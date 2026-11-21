#include "audio_beat.h"

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
double Seconds() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return double(c.QuadPart) / double(f.QuadPart); }
constexpr float kDecay = .2f;  // seconds
}

void AudioBeat::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void AudioBeat::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    level_ = 0; prevLevel_ = 0; loud_ = 0;
}

float AudioBeat::Pulse() const {
    const double now = Seconds();
    float l = level_.load(); double at = levelTime_.load();
    if (now < at) { l = prevLevel_.load(); at = prevTime_.load(); }  // the newest pulse has not started yet
    if (l <= 0 || now < at) return 0;
    return l * std::exp(-float(now - at) / kDecay);
}

void AudioBeat::Post(float strength, double at) {
    // A pulse weaker than what is still showing at that moment is dropped.
    const double now = Seconds();
    const float showing = level_.load() * std::exp(-float(std::max(0.0, at - levelTime_.load())) / kDecay);
    if (strength <= showing && at - levelTime_.load() < .15) return;
    if (now >= levelTime_.load()) { prevLevel_ = level_.load(); prevTime_ = levelTime_.load(); }
    level_ = strength; levelTime_ = at;
}

void AudioBeat::Loop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop_) {
        // (Re)connect to the default output; on any failure wait and try again (device change, no device).
        ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device; ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
                  SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                  SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) &&
                  SUCCEEDED(client->GetMixFormat(&format)) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, format, nullptr)) &&
                  SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
        if (!ok) { if (format) CoTaskMemFree(format); for (int i = 0; i < 20 && !stop_; ++i) Sleep(100); continue; }
        const UINT channels = format->nChannels, rate = format->nSamplesPerSec;
        bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            isFloat = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;  // KSDATAFORMAT_SUBTYPE_* GUIDs carry the format tag
        }
        const UINT bits = format->wBitsPerSample;
        CoTaskMemFree(format);
        BandSplitter bands; bands.Reset(rate);
        // Grid beats are posted 60 ms ahead and carry their exact time, so the pulse lands on the beat no matter how
        // the capture packets are batched.
        BeatTracker tracker(.06f); float peak = 1e-6f, loud = 0;
        while (!stop_) {
            Sleep(8);
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet))) break;  // device gone: reconnect
            while (packet && !stop_) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 devicePos = 0, qpcPos = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devicePos, &qpcPos))) { packet = 0; break; }
                // Time of the packet's first frame (QPC in 100 ns), or now when the driver gives none.
                const double packetStart = qpcPos ? double(qpcPos) * 1e-7 : Seconds() - double(frames) / double(rate);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                for (UINT32 f = 0; f < frames; ++f) {
                    float mono = 0;
                    if (!silent) {
                        for (UINT c = 0; c < channels; ++c) {
                            if (isFloat && bits == 32) { float v; std::memcpy(&v, data + (size_t(f) * channels + c) * 4, 4); mono += v; }
                            else if (bits == 16) { int16_t v; std::memcpy(&v, data + (size_t(f) * channels + c) * 2, 2); mono += v / 32768.f; }
                            else if (bits == 32) { int32_t v; std::memcpy(&v, data + (size_t(f) * channels + c) * 4, 4); mono += float(v / 2147483648.0); }
                        }
                        mono /= float(channels ? channels : 1);
                    }
                    float low = 0, high = 0;
                    if (bands.Push(mono, low, high)) {
                        peak = std::max(low, peak * .9985f);  // ~6 s fall
                        loud += (std::sqrt(low / std::max(peak, 1e-9f)) - loud) * .12f; loud_ = loud;
                        const float strength = tracker.Feed(low, high, bands.BlockSeconds());
                        if (strength > 0) Post(strength, packetStart + double(f + 1) / double(rate) + tracker.PulseOffset());
                    }
                }
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packet))) { packet = 0; break; }
            }
        }
        client->Stop();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace nowplaying
