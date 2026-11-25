// Diagnostic for the Now Playing beat detector.
//   docky-beat-probe [seconds]
//   docky-beat-probe --file <audio> [--lock <lock> <release>] [--labels <file>] [--sensitivity <0-100>]
// Labels are whitespace-separated seconds and event names, one event per line.
#include "../src/now_playing/music_analysis.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nowplaying;
using Clock = std::chrono::steady_clock;
constexpr const char* kKinds[] = {"onset", "kick", "snare", "hihat", "beat", "downbeat", "drop"};

struct Distribution {
    std::vector<double> values;
    void Print(const char* label) {
        if (values.empty()) return;
        std::sort(values.begin(), values.end());
        std::printf("%s p50 %.3f ms, p95 %.3f ms, max %.3f ms (n=%zu)\n", label,
            values[values.size() / 2], values[(values.size() - 1) * 95 / 100], values.back(), values.size());
    }
};

static void Compare(const char* name, std::vector<double> observed, std::vector<double> reference, double tolerance) {
    std::sort(observed.begin(), observed.end());
    std::sort(reference.begin(), reference.end());
    Distribution error;
    size_t next = 0;
    for (double time : observed) {
        while (next < reference.size() && reference[next] < time - tolerance) ++next;
        if (next == reference.size() || reference[next] > time + tolerance) continue;
        if (next + 1 < reference.size() && std::abs(reference[next + 1] - time) < std::abs(reference[next] - time)) ++next;
        error.values.push_back(std::abs(time - reference[next++]) * 1000);
    }
    const auto matched = error.values.size();
    std::printf("%s TP=%zu FP=%zu FN=%zu precision=%.3f recall=%.3f\n", name, matched,
        observed.size() - matched, reference.size() - matched, double(matched) / std::max<size_t>(1, observed.size()),
        double(matched) / std::max<size_t>(1, reference.size()));
    error.Print("  absolute timing error");
}

static int RunFile(const wchar_t* path, float lockAt, float releaseAt, const wchar_t* labels, int sensitivity) {
    std::array<std::vector<double>, std::size(kKinds)> references, detected;
    if (labels) {
        std::ifstream input{std::filesystem::path(labels)};
        double at;
        std::string kind;
        if (!input) { std::printf("cannot open labels\n"); return 2; }
        while (input >> at >> kind) {
            const auto match = std::find_if(std::begin(kKinds), std::end(kKinds), [&](auto name) { return kind == name; });
            if (match == std::end(kKinds) || !std::isfinite(at) || at < 0) return 2;
            references[match - std::begin(kKinds)].push_back(at);
        }
        if (!input.eof()) return 2;
    }
    if (FAILED(MFStartup(MF_VERSION))) return 2;
    struct MfShutdown { ~MfShutdown() { MFShutdown(); } } shutdown;
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
    if (!channels || rate < 8000 || rate > 384000) return 2;
    nowplaying::BandSplitter bands; bands.Reset(rate);
    nowplaying::BeatTracker tracker(0.f);  // offline: no capture delay to make up for
    MusicAnalyzer analyzer;
    analyzer.Configure(rate, 0);
    analyzer.SetSensitivity(sensitivity);
    StereoFold stereo;
    if (lockAt > 0) { tracker.SetLockThresholds(lockAt, releaseAt); analyzer.SetLockThresholds(lockAt, releaseAt); }
    Distribution dsp;
    double dspBlock = 0, firstLock = -1, oldFirstLock = -1, referenceBpm = 0;
    unsigned tempoBlocks = 0, octaveErrors = 0, oldTempoBlocks = 0, oldOctaveErrors = 0;
    auto& beatLabels = references[size_t(RhythmKind::Beat)];
    if (beatLabels.size() >= 4) {
        std::sort(beatLabels.begin(), beatLabels.end());
        std::vector<double> intervals;
        for (size_t i = 1; i < beatLabels.size(); ++i)
            if (beatLabels[i] > beatLabels[i - 1]) intervals.push_back(beatLabels[i] - beatLabels[i - 1]);
        const auto mid = intervals.begin() + intervals.size() / 2;
        if (!intervals.empty()) { std::nth_element(intervals.begin(), mid, intervals.end()); referenceBpm = 60 / *mid; }
    }
    const auto octaveError = [&](float bpm) {
        const double ratio = bpm / referenceBpm;
        return std::abs(ratio - .5) < .04 || std::abs(ratio - 2) < .12;
    };
    uint64_t samples = 0;
    MusicFrame frame;
    std::vector<double> pulses, lockedPulses, jitter;
    double t = 0, lastLocked = -1;
    int lockedBlocks = 0, blocks = 0;
    const double block = bands.BlockSeconds();
    for (;;) {
        DWORD flags = 0; ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return 2;
        BYTE* data = nullptr; DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return 2;
        const size_t frames = length / (sizeof(float) * channels);
        for (size_t f = 0; f < frames; ++f) {
            float mono = 0, left = 0, right = 0, extra = 0;
            for (UINT32 c = 0; c < channels; ++c) {
                float v;
                std::memcpy(&v, data + (f * channels + c) * sizeof(float), sizeof(float));
                v = std::isfinite(v) ? std::clamp(v, -4.f, 4.f) : 0.f;
                mono += v;
                if (c == 0) left = v;
                else if (c == 1) right = v;
                else extra += v;
            }
            ++samples;
            const auto begin = Clock::now();
            float folded = channels > 1 ? stereo.Push(left, right) : left;
            if (channels > 2) folded = .8f * folded + .2f * extra / (channels - 2);
            const bool ready = analyzer.Push(folded, double(samples) / rate, frame);
            dspBlock += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (ready) {
                dsp.values.push_back(dspBlock);
                dspBlock = 0;
                if (firstLock < 0 && frame.period > 0) firstLock = frame.at;
                if (referenceBpm && frame.period > 0) { ++tempoBlocks; octaveErrors += octaveError(frame.bpm); }
                for (unsigned i = 0; i < frame.count; ++i) {
                    const auto& event = frame.events[i];
                    const auto kind = static_cast<size_t>(event.kind);
                    detected[kind].push_back(event.at);
                    std::printf("new %.4fs %s strength=%.2f confidence=%.2f bpm=%.2f\n",
                        event.at, kKinds[kind], event.strength, event.confidence, frame.bpm);
                }
            }
            float low, high;
            if (bands.Push(mono / float(channels), low, high)) {
                t += block;
                const float s = tracker.Feed(low, high, float(block));
                ++blocks;
                if (tracker.Locked()) {
                    ++lockedBlocks;
                    if (oldFirstLock < 0) oldFirstLock = t;
                    if (referenceBpm) { ++oldTempoBlocks; oldOctaveErrors += octaveError(tracker.Bpm()); }
                } else lastLocked = -1;
                if (s > 0 && tracker.Locked()) {  // pulse time = block end + the tracker's offset; jitter against the tempo
                    const double at = t + tracker.PulseOffset(), period = 60.0 / tracker.Bpm();
                    if (lastLocked >= 0 && at - lastLocked < period * 1.5) jitter.push_back(at - lastLocked - period);
                    lastLocked = at;
                    lockedPulses.push_back(at);
                }
                if (s > 0) { pulses.push_back(t); std::printf("old %7.2fs pulse %.2f%s\n", t, s, tracker.Locked() ? "" : "  (free)"); }
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
    std::printf("first lock seconds old %.3f new %.3f (-1 = unlocked), new final tempo %.2f BPM\n", oldFirstLock, firstLock, frame.bpm);
    if (referenceBpm) std::printf("median reference %.2f BPM, octave errors in locked hops old %u/%u new %u/%u\n",
        referenceBpm, oldOctaveErrors, oldTempoBlocks, octaveErrors, tempoBlocks);
    dsp.Print("new DSP per hop (sample-level instrumentation overhead included)");
    if (labels) {
        for (size_t i = 0; i < std::size(kKinds); ++i)
            Compare(kKinds[i], detected[i], references[i], i == size_t(RhythmKind::Drop) ? .5 : .05);
        Compare("old locked beat", lockedPulses, references[size_t(RhythmKind::Beat)], .05);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc > 2 && std::wcscmp(argv[1], L"--file") == 0) {  // optional: --lock <lock> <release>
        const wchar_t* labels = nullptr;
        float lock = 0, release = 0;
        int sensitivity = 50;
        for (int i = 3; i < argc;) {
            const std::wstring option = argv[i++];
            if (option == L"--lock" && i + 1 < argc) { lock = float(_wtof(argv[i++])); release = float(_wtof(argv[i++])); }
            else if (option == L"--sensitivity" && i < argc) sensitivity = _wtoi(argv[i++]);
            else if (option == L"--labels" && i < argc) labels = argv[i++];
            else { CoUninitialize(); return 2; }
        }
        const int r = RunFile(argv[2], lock, release, labels, sensitivity);
        CoUninitialize(); return r;
    }
    const int seconds = argc > 1 ? std::clamp(_wtoi(argv[1]), 1, 3600) : 10;
    nowplaying::AudioBeat beat;
    if (argc == 4 && std::wcscmp(argv[2], L"--sensitivity") == 0) beat.SetSensitivity(_wtoi(argv[3]));
    beat.Start();
    Distribution capture, analysis, delivery;
    MusicFrame frame;
    unsigned frames = 0, events = 0;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < DWORD(seconds) * 1000) {
        while (beat.Pop(frame)) {
            if (!frame.captured) continue;
            ++frames;
            capture.values.push_back((frame.captured - frame.at) * 1000);
            analysis.values.push_back((frame.analyzed - frame.captured) * 1000);
            delivery.values.push_back((frame.published - frame.analyzed) * 1000);
            for (unsigned i = 0; i < frame.count; ++i) {
                ++events;
                const auto& event = frame.events[i];
                std::printf("audio_qpc=%.6f %s confidence=%.2f bpm=%.2f\n", event.at,
                    kKinds[static_cast<size_t>(event.kind)], event.confidence, frame.bpm);
            }
        }
        Sleep(5);
    }
    const auto mode = beat.Mode();
    beat.Stop();
    std::printf("%u frames, %u events, %u overruns in %d s, capture mode %s\n", frames, events, beat.Overruns(),
        seconds, mode == CaptureMode::Events ? "events" : mode == CaptureMode::Polling ? "polling" : "disconnected");
    capture.Print("audio timestamp to packet read");
    analysis.Print("packet read to analysis");
    delivery.Print("analysis to publication");
    CoUninitialize();
    return 0;
}
