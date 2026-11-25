#include "audio_beat.h"
#include "music_analysis.h"

#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace nowplaying {
namespace {
constexpr GUID WaveSubtype(DWORD tag) { return {tag, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}}; }

double Seconds() {
    static const auto frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) / frequency;
}
bool IsDefault(IMMDeviceEnumerator* enumerator, const wchar_t* id) {
    ComPtr<IMMDevice> device;
    LPWSTR current = nullptr;
    const bool same = SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->GetId(&current)) && id && std::wcscmp(current, id) == 0;
    CoTaskMemFree(current);
    return same;
}

float Decode(const BYTE* data, UINT bits, bool floating) {
    if (floating) {
        float sample;
        std::memcpy(&sample, data, sizeof(sample));
        return std::isfinite(sample) ? std::clamp(sample, -4.f, 4.f) : 0.f;
    }
    switch (bits) {
        case 8: return (int(data[0]) - 128) / 128.f;
        case 16: { int16_t sample; std::memcpy(&sample, data, 2); return sample / 32768.f; }
        case 24: return (int(data[0]) + 256 * int(data[1]) + 65536 * int(int8_t(data[2]))) / 8388608.f;
        case 32: { int32_t sample; std::memcpy(&sample, data, 4); return float(sample / 2147483648.0); }
        default: return 0.f;
    }
}
}

void AudioBeat::Start() {
    if (thread_.joinable()) return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return;
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void AudioBeat::Stop() {
    stop_ = true;
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    MusicFrame discarded;
    while (queue_.Pop(discarded)) {}
}

void AudioBeat::Loop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return;
    const HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    unsigned generation = 0;
    while (!stop_) {
        ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device; ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;
        LPWSTR id = nullptr;
        const bool found = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && SUCCEEDED(device->GetId(&id));
        auto connect = [&](bool eventDriven) {
            capture.Reset();
            client.Reset();
            CoTaskMemFree(format);
            format = nullptr;
            const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | (eventDriven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
            return found && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf()))) && SUCCEEDED(client->GetMixFormat(&format)) &&
                SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format, nullptr)) &&
                (!eventDriven || SUCCEEDED(client->SetEventHandle(ready))) &&
                SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
        };
        const bool eventDriven = ready && connect(true);
        if (!eventDriven && !connect(false)) {
            CoTaskMemFree(format);
            CoTaskMemFree(id);
            WaitForSingleObject(stopEvent_, 500);
            continue;
        }
        const UINT channels = format->nChannels, rate = format->nSamplesPerSec;
        bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            isFloat = ext->SubFormat == WaveSubtype(WAVE_FORMAT_IEEE_FLOAT);
            isPcm = ext->SubFormat == WaveSubtype(WAVE_FORMAT_PCM);
        }
        const UINT bits = format->wBitsPerSample, stride = format->nBlockAlign;
        const bool valid = channels && rate >= 8000 && rate <= 384000 && stride >= channels * (bits / 8) &&
            (isFloat ? bits == 32 : isPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32));
        CoTaskMemFree(format);
        if (!valid) { client->Stop(); CoTaskMemFree(id); WaitForSingleObject(stopEvent_, 500); continue; }
        bool waitForEvent = eventDriven;
        mode_ = eventDriven ? CaptureMode::Events : CaptureMode::Polling;
        MusicAnalyzer analyzer;
        analyzer.Configure(rate);
        StereoFold stereo;
        MusicFrame frame;
        const HANDLE handles[] = {stopEvent_, ready};
        double nextCheck = Seconds() + .5, previousEnd = 0, lastPacket = 0;
        UINT64 previousPosition = 0;
        bool havePosition = false, connected = true;
        unsigned missedWakeups = 0;
        unsigned reset = reset_.load();
        auto resetStream = [&] {
            analyzer.Reset();
            stereo = {};
            previousEnd = 0;
            havePosition = false;
            MusicFrame discontinuity;
            discontinuity.generation = ++generation;
            discontinuity.at = discontinuity.published = Seconds();
            queue_.Push(discontinuity);
        };
        resetStream();
        while (!stop_ && connected) {
            const DWORD wait = waitForEvent ? WaitForMultipleObjects(2, handles, FALSE, 50)
                                           : WaitForSingleObject(stopEvent_, 5);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            const double now = Seconds();
            if (now >= nextCheck) {
                if (!IsDefault(enumerator.Get(), id)) break;
                nextCheck = now + .5;
            }
            if (reset != reset_.load() || (lastPacket && now - lastPacket > .25)) {
                reset = reset_.load();
                resetStream();
                lastPacket = 0;
            }
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) break;  // device gone: reconnect
            if (waitForEvent) {
                if (wait == WAIT_OBJECT_0 + 1) missedWakeups = 0;
                else if (packet && ++missedWakeups >= 2) {
                    // Some endpoint drivers accept the event flag but never signal it.
                    waitForEvent = false;
                    mode_ = CaptureMode::Polling;
                }
            }
            while (packet && !stop_) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 devicePos = 0, qpcPos = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devicePos, &qpcPos))) { connected = false; break; }
                const double captured = Seconds();
                lastPacket = captured;
                const double reported = double(qpcPos) * 1e-7;
                const bool timestampOk = qpcPos && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
                    std::abs(reported - captured) < 1.;
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ||
                    (timestampOk && havePosition && devicePos != previousPosition)) resetStream();
                const double duration = double(frames) / rate;
                // Keep the sample clock continuous across batched packets and fall back to arrival
                // time only when there is no usable hardware timestamp or prior anchor.
                if (timestampOk && previousEnd && std::abs(reported - previousEnd) > .035) resetStream();
                const double packetStart = previousEnd ? previousEnd + (timestampOk
                    ? std::clamp(reported - previousEnd, -.0001, .0001) : 0)
                    : timestampOk ? reported : captured - duration;
                previousEnd = packetStart + duration;
                havePosition = timestampOk;
                previousPosition = devicePos + frames;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                analyzer.SetSensitivity(sensitivity_.load(std::memory_order_relaxed));
                for (UINT32 f = 0; f < frames; ++f) {
                    float mono = 0;
                    if (!silent) {
                        const BYTE* sample = data + size_t(f) * stride;
                        const float left = Decode(sample, bits, isFloat);
                        mono = left;
                        if (channels > 1) {
                            mono = stereo.Push(left, Decode(sample + bits / 8, bits, isFloat));
                            if (channels > 2) {
                                float extra = 0;
                                for (UINT c = 2; c < channels; ++c) extra += Decode(sample + c * (bits / 8), bits, isFloat);
                                mono = .8f * mono + .2f * extra / (channels - 2);
                            }
                        }
                    }
                    if (analyzer.Push(mono, packetStart + double(f + 1) / rate, frame)) {
                        frame.generation = generation;
                        frame.captured = captured;
                        frame.analyzed = Seconds();
                        for (unsigned i = 0; i < frame.count; ++i) {
                            auto& event = frame.events[i];
                            event.captured = captured;
                            event.analyzed = frame.analyzed;
                            event.sampleEnd = frame.at;
                        }
                        frame.published = Seconds();
                        for (unsigned i = 0; i < frame.count; ++i) frame.events[i].published = frame.published;
                        queue_.Push(frame);
                    }
                }
                if (FAILED(capture->ReleaseBuffer(frames)) || FAILED(capture->GetNextPacketSize(&packet))) { connected = false; break; }
            }
        }
        client->Stop();
        mode_ = CaptureMode::Disconnected;
        CoTaskMemFree(id);
        resetStream();
    }
    if (ready) CloseHandle(ready);
    CoUninitialize();
}

}  // namespace nowplaying
