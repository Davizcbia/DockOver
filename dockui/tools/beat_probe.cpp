// Diagnostic for the Now Playing beat detector.
//   docky-beat-probe [seconds]          live: WASAPI loopback for N seconds, prints each pulse
//   docky-beat-probe --file <audio> [--lock <lock> <release>]   offline: decodes the file (Media Foundation) and runs the same tracker on it,
//                                       printing pulses, the tempo and how regular the pulses are
#include "../src/now_playing/audio_beat.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

static int RunFile(const wchar_t* path, float lockAt, float releaseAt) {
    if (FAILED(MFStartup(MF_VERSION))) return 2;
    ComPtr<IMFSourceReader> reader; ComPtr<IMFMediaType> want, got;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader))) { std::printf("cannot open\n"); return 2; }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.Get())) ||
        FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) { std::printf("no float audio\n"); return 2; }
    const UINT32 rate = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    const UINT32 channels = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
    nowplaying::BandSplitter bands; bands.Reset(rate);
    nowplaying::BeatTracker tracker(0.f);  // offline: no capture delay to make up for
    if (lockAt > 0) tracker.SetLockThresholds(lockAt, releaseAt);
    std::vector<double> pulses, lockedPulses, jitter; double t = 0; int lockedBlocks = 0, blocks = 0; const double block = bands.BlockSeconds();
    for (;;) {
        DWORD flags = 0; ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer; sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr; DWORD length = 0; buffer->Lock(&data, nullptr, &length);
        const size_t frames = length / (sizeof(float) * channels);
        for (size_t f = 0; f < frames; ++f) {
            float mono = 0;
            for (UINT32 c = 0; c < channels; ++c) { float v; std::memcpy(&v, data + (f * channels + c) * sizeof(float), sizeof(float)); mono += v; }
            float low, high;
            if (bands.Push(mono / float(channels), low, high)) {
                t += block;
                const float s = tracker.Feed(low, high, float(block));
                ++blocks; if (tracker.Locked()) ++lockedBlocks;
                if (s > 0 && tracker.Locked()) {  // pulse time = block end + the tracker's offset; jitter against the tempo
                    const double at = t + tracker.PulseOffset(), period = 60.0 / tracker.Bpm();
                    if (!lockedPulses.empty() && at - lockedPulses.back() < period * 1.5) jitter.push_back(at - lockedPulses.back() - period);
                    lockedPulses.push_back(at);
                } else if (s > 0) lockedPulses.clear();
                if (s > 0) { pulses.push_back(t); std::printf("%7.2fs pulse %.2f%s\n", t, s, tracker.Locked() ? "" : "  (free)"); }
            }
        }
        buffer->Unlock();
    }
    // Regularity: intervals between pulses after the first 10 s, relative to the tracked period.
    double sum = 0, sq = 0; int n = 0;
    for (size_t i = 1; i < pulses.size(); ++i) if (pulses[i - 1] > 10) { const double d = pulses[i] - pulses[i - 1]; sum += d; sq += d * d; ++n; }
    const double mean = n ? sum / n : 0, sd = n ? std::sqrt(std::max(0.0, sq / n - mean * mean)) : 0;
    std::printf("locks %d, tempo changes %d, phase jumps %d\n", tracker.GetStats().locks, tracker.GetStats().tempoChanges, tracker.GetStats().phaseJumps);
    std::printf("%zu pulses in %.1f s, locked %.0f%% of the time, tempo %.1f BPM (locked %d), mean interval %.3f s, sd %.3f s, locked jitter %.1f ms\n", pulses.size(), t, blocks ? 100.0 * lockedBlocks / blocks : 0.0, tracker.Bpm(), tracker.Locked() ? 1 : 0, mean, sd,
                jitter.empty() ? 0.0 : 1000 * std::sqrt([&] { double q = 0; for (double j : jitter) q += j * j; return q / double(jitter.size()); }()));
    MFShutdown();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc > 2 && std::wcscmp(argv[1], L"--file") == 0) {  // optional: --lock <lock> <release>
        const bool lock = argc > 5 && std::wcscmp(argv[3], L"--lock") == 0;
        const int r = RunFile(argv[2], lock ? float(_wtof(argv[4])) : 0.f, lock ? float(_wtof(argv[5])) : 0.f);
        CoUninitialize(); return r;
    }
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 10;
    nowplaying::AudioBeat beat; beat.Start();
    float last = 0; int pulses = 0; const DWORD start = GetTickCount();
    while (GetTickCount() - start < DWORD(seconds) * 1000) {
        const float p = beat.Pulse();
        if (p > last + .1f) { ++pulses; std::printf("%6.2fs pulse %.2f\n", (GetTickCount() - start) / 1000.0, p); }
        last = p; Sleep(5);
    }
    beat.Stop();
    std::printf("%d pulses in %d s\n", pulses, seconds);
    CoUninitialize();
    return 0;
}
