#pragma once
// Beat pulse for the lyrics: reads what the default audio output is playing through WASAPI loopback (the standard
// Windows capture path audio visualisers use; no hooks, nothing injected into the player, nothing stored or sent).
// BandSplitter turns samples into per-10 ms bass and treble energies; BeatTracker finds onsets (log-energy flux over an
// adaptive threshold), learns the tempo from them and then pulses on the beat grid, slightly ahead of the captured
// audio to make up for the capture delay. Started and stopped by the view (only while lyrics are shown); reconnects
// when the output device changes.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <thread>
#include <vector>

namespace nowplaying {

class AudioBeat {
public:
    AudioBeat() = default;
    ~AudioBeat() { Stop(); }
    AudioBeat(const AudioBeat&) = delete;
    AudioBeat& operator=(const AudioBeat&) = delete;

    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }
    // 0..1: jumps up on a beat and decays over ~0.2 s. Read at render time.
    float Pulse() const;
    // 0..1: how loud the bass is relative to its recent peak (smoothed); drives the flow speed.
    float Level() const { return loud_.load(); }

private:
    void Loop();
    std::atomic<bool> stop_{false};
    // Newest pulse (may start slightly in the future: grid beats are posted ahead with their exact time) and the one
    // before it, which keeps decaying until the newest one starts.
    std::atomic<float> level_{0}, prevLevel_{0};
    std::atomic<double> levelTime_{0}, prevTime_{0};  // seconds (QPC)
    void Post(float strength, double at);
    std::atomic<float> loud_{0};
    std::thread thread_;
};

// Per-sample filters: bass = 4th-order low-pass at 130 Hz after a 35 Hz high-pass (kick and bass line); treble =
// above ~2 kHz (snare and hat attacks). Push() returns true each 10 ms with the block's mean energies.
class BandSplitter {
public:
    void Reset(unsigned rate) {
        rate_ = rate; block_ = std::max(1u, rate / 100); n_ = 0; low_ = high_ = 0;
        lp1_ = Biquad::LowPass(130.f, float(rate)); lp2_ = lp1_;
        hp_ = Biquad::HighPass(35.f, float(rate)); hpTreble_ = Biquad::HighPass(2000.f, float(rate));
    }
    bool Push(float x, float& low, float& high) {
        const float b = lp2_.Run(lp1_.Run(hp_.Run(x)));
        const float t = hpTreble_.Run(x);
        low_ += b * b; high_ += t * t;
        if (++n_ < block_) return false;
        low = low_ / float(block_); high = high_ / float(block_);
        low_ = high_ = 0; n_ = 0;
        return true;
    }
    float BlockSeconds() const { return rate_ ? float(block_) / float(rate_) : .01f; }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float Run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        static Biquad Make(float f, float rate, bool high) {
            const float w = 2 * 3.14159265f * f / rate, c = std::cos(w), alpha = std::sin(w) / (2 * .7071f), a0 = 1 + alpha;
            Biquad q;
            if (high) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
            else { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = q.b0; }
            q.a1 = -2 * c / a0; q.a2 = (1 - alpha) / a0;
            return q;
        }
        static Biquad LowPass(float f, float rate) { return Make(f, rate, false); }
        static Biquad HighPass(float f, float rate) { return Make(f, rate, true); }
    };
    Biquad lp1_, lp2_, hp_, hpTreble_;
    unsigned rate_ = 0, block_ = 1, n_ = 0;
    float low_ = 0, high_ = 0;
};

// Pure beat tracker (unit tested). Feed one block's bass and treble energy; returns the strength (0..1) of a beat
// pulse to start now, or 0.
//  1. Onset strength: log-compressed energy rise over the previous 30 ms (bass + a quarter of treble). Onsets are its peaks
//     above mean + 1.4 x deviation of the last ~1.5 s, at least 0.15 s apart (one block of delay).
//  2. Tempo (every 0.25 s): autocorrelation of the last 6 s of onset strength over 75-200 BPM, with its double and
//     triple lags added (so a 3:2 cross-rhythm does not win) and a gentle preference for ~120 BPM; phase = the offset whose beat grid collects the most onset strength.
//     Locks when the rhythm is clear (normalised autocorrelation >= 0.2), lets go below 0.12.
//  3. Locked: pulses come from the grid, `lead` seconds early (makes up for the capture delay); onsets near the grid
//     nudge it (also hits up to 0.1 s before a beat; they never pulse on their own). After three grid beats with no onset (a break, a quiet bridge) the grid stays silent until hits return.
//     Without a lock only clear onsets pulse.
class BeatTracker {
public:
    explicit BeatTracker(float lead = .03f) : lead_(lead) {}
    // Lock / release thresholds of the normalised autocorrelation (diagnostics may try others).
    void SetLockThresholds(float lock, float release) { lockAt_ = lock; releaseAt_ = release; }
    // When the pulse just returned belongs, in seconds relative to the end of the block just fed: negative for a
    // detected onset (it was in an earlier block), the grid beat's own time for a grid pulse (up to `lead` ahead).
    double PulseOffset() const { return pulseOffset_; }

    float Feed(float low, float high, float dt) {
        t_ += double(dt); dt_ = dt;
        const bool silent = low + high < 1e-9f;
        const float l = std::log1p(low * 1e4f), h = std::log1p(high * 1e4f);
        float flux = 0;
        if (!silent && filled_ == 3) {
            const float pl = (hl_[0] + hl_[1] + hl_[2]) / 3, ph = (hh_[0] + hh_[1] + hh_[2]) / 3;
            flux = std::max(0.f, l - pl) + .25f * std::max(0.f, h - ph);
        }
        hl_[pos_] = l; hh_[pos_] = h; pos_ = (pos_ + 1) % 3; filled_ = silent ? 0 : std::min(3, filled_ + 1);
        const float k = std::min(1.f, dt / 1.5f);
        mean_ += (flux - mean_) * k; dev_ += (std::fabs(flux - mean_) - dev_) * k;
        const float threshold = mean_ + 1.4f * dev_ + .04f;
        history_[head_] = flux; head_ = (head_ + 1) % kHistory; count_ = std::min(kHistory, count_ + 1);

        float out = 0;
        const double onsetTime = t_ - double(dt);  // peak picking on the previous block
        if (prev_ > prevThreshold_ && prev_ >= prev2_ && prev_ > flux && onsetTime - lastOnset_ >= .15) {
            lastOnset_ = onsetTime;
            const float s = std::clamp(.35f + .45f * (prev_ / prevThreshold_ - 1.f), .35f, 1.f);
            out = OnOnset(onsetTime, s);
            pulseOffset_ = -double(dt);
        }
        prev2_ = prev_; prev_ = flux; prevThreshold_ = threshold;
        if (silent) { locked_ = false; count_ = 0; misses_ = 0; memory_ = 0; return out; }  // silence: next track, forget the tempo
        if (++sinceEstimate_ >= 25 && count_ >= 300) { sinceEstimate_ = 0; Estimate(); }

        if (locked_ && t_ >= gridT_ + period_ - lead_) {
            if (!matched_) misses_ = std::min(misses_ + 1, 3);
            gridT_ += period_; matched_ = nextMatched_; nextMatched_ = false;
            if (matched_) misses_ = 0;
            if (misses_ < 3) { out = std::max(out, std::clamp(strength_ * (misses_ ? .75f : 1.f), .4f, 1.f)); pulseOffset_ = gridT_ - t_; }
        }
        return out;
    }

    float Bpm() const { return locked_ ? 60.f / period_ : 0.f; }
    struct Stats { int locks = 0, tempoChanges = 0, phaseJumps = 0; };
    const Stats& GetStats() const { return stats_; }
    bool Locked() const { return locked_; }

private:
    static constexpr int kHistory = 600;  // 6 s of 10 ms blocks

    float OnOnset(double t, float s) {
        if (!locked_) return s >= .5f ? s * .8f : 0.f;  // no tempo yet: only clear hits, a little softer
        const float e = float(t - gridT_);  // relative to the grid beat just pulsed
        if (std::fabs(e) < .1f) { Hit(s); gridT_ += .1 * e; return 0; }
        const double next = gridT_ + period_;
        if (t >= next - .1) {  // a hit just before the next grid beat: it counts for that beat and pulls the grid a little
            nextMatched_ = true; misses_ = 0; strength_ += (s - strength_) * .3f;
            gridT_ += .1 * (t - next);
            return 0;
        }
        return 0.f;  // off the beat: ignored while locked (it would make the pulse look random)
    }
    void Hit(float s) { matched_ = true; misses_ = 0; strength_ += (s - strength_) * .3f; }

    void Estimate() {
        const int n = count_;
        auto& x = x_; auto& c = c_;  // reused between estimates (no allocation every 0.25 s)
        x.resize(size_t(n)); c.resize(size_t(n));
        for (int i = 0; i < n; ++i) x[size_t(i)] = history_[size_t((head_ - n + i + kHistory) % kHistory)];
        float mean = 0; for (float v : x) mean += v; mean /= float(n);
        for (int i = 0; i < n; ++i) c[size_t(i)] = x[size_t(i)] - mean;
        double ac0 = 0; for (float v : c) ac0 += double(v) * v;
        if (ac0 <= 1e-9) return;
        ac0 /= n;
        const float bd = std::max(1e-3f, dt_);
        const int minLag = std::max(2, int(.3f / bd)), maxLag = int(.8f / bd);
        if (3 * maxLag + 2 >= n) return;
        auto ac = [&](int lag) { double s = 0; for (int i = lag; i < n; ++i) s += double(c[size_t(i)]) * c[size_t(i - lag)]; return float(s / (n - lag) / ac0); };
        auto& raw = raw_; raw.assign(size_t(3 * maxLag + 3), 0.f);
        for (int lag = minLag - 1; lag <= 3 * maxLag + 2; ++lag) raw[size_t(lag)] = ac(lag);
        auto& score = score_; score.assign(size_t(maxLag + 1), -1e9f);
        int best = -1;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            const float octaves = std::log2(float(lag) * bd / .5f);
            score[size_t(lag)] = (raw[size_t(lag)] + .5f * raw[size_t(2 * lag)] + .33f * raw[size_t(3 * lag)]) * std::exp(-.5f * octaves * octaves / (.9f * .9f));
            if (best < 0 || score[size_t(lag)] > score[size_t(best)]) best = lag;
        }
        // Stay with the tempo already locked (or, after a break, the one remembered from before it) unless another is
        // clearly and repeatedly better (no flipping between related tempos such as half, double or 3:2).
        if (!locked_ && memory_ > 0) {
            const int cur = int(memory_ / bd + .5f), lo = std::max(minLag, int(cur * .94f)), hi = std::min(maxLag, int(cur * 1.06f + 1));
            int near = -1;
            for (int lag = lo; lag <= hi; ++lag) if (near < 0 || score[size_t(lag)] > score[size_t(near)]) near = lag;
            if (near >= 0 && score[size_t(near)] >= .7f * score[size_t(best)]) best = near;
        }
        if (locked_) {
            const int cur = int(period_ / bd + .5f), lo = std::max(minLag, int(cur * .94f)), hi = std::min(maxLag, int(cur * 1.06f + 1));
            int near = -1;
            for (int lag = lo; lag <= hi; ++lag) if (near < 0 || score[size_t(lag)] > score[size_t(near)]) near = lag;
            if (near >= 0 && best != near) {
                if (score[size_t(best)] < 1.3f * score[size_t(near)] || ++switchVotes_ < 3) best = near;
                else switchVotes_ = 0;
            } else switchVotes_ = 0;
        }
        const float confidence = raw[size_t(best)];  // of the chosen lag
        // Sub-block period from a parabola through the neighbours.
        const float a = raw[size_t(best - 1)], b = raw[size_t(best)], cc = raw[size_t(best + 1)];
        const float shift = (a - 2 * b + cc) < 0 ? std::clamp(.5f * (a - cc) / (a - 2 * b + cc), -.5f, .5f) : 0.f;
        const float period = (float(best) + shift) * bd;
        if (!locked_ && confidence < lockAt_) return;
        if (locked_ && confidence < releaseAt_) { locked_ = false; return; }
        // Phase: the grid offset collecting the most onset strength (recent beats count more).
        const float lagBlocks = period / bd;
        int bestPhase = 0; float bestSum = -1;
        for (int ph = 0; ph < int(lagBlocks); ++ph) {
            float sum = 0, w = 1;
            for (float pos = float(n - 1 - ph); pos >= 1; pos -= lagBlocks, w *= .85f) {
                const int i = int(pos); const float f = pos - float(i);
                sum += w * (x[size_t(i)] * (1 - f) + x[size_t(std::min(n - 1, i + 1))] * f);
            }
            if (sum > bestSum) { bestSum = sum; bestPhase = ph; }
        }
        const double beat = t_ - double(bestPhase) * bd;
        if (!locked_) {
            ++stats_.locks;
            locked_ = true; period_ = period; memory_ = period; gridT_ = beat; matched_ = true; misses_ = 0; jumps_ = 0;
            return;
        }
        if (std::fabs(period - period_) / period_ >= .06f) ++stats_.tempoChanges;
        period_ = std::fabs(period - period_) / period_ < .06f ? period_ + (period - period_) * .1f : period;
        memory_ = period_;
        double diff = std::fmod(beat - gridT_, double(period_));
        if (diff > period_ / 2) diff -= period_; else if (diff < -period_ / 2) diff += period_;
        // Small phase errors: correct the phase and (like a phase-locked loop) the period, so a slightly-off tempo does
        // not drift into phase jumps.
        if (std::fabs(diff) < .1) { gridT_ += .2 * diff; period_ += float(.03 * diff); memory_ = period_; jumps_ = 0; }
        else {  // a clear phase change only when three estimates in a row point to the same new phase
            if (jumps_ > 0 && std::fabs(diff - pendingDiff_) < .04) ++jumps_; else { jumps_ = 1; pendingDiff_ = diff; }
            if (jumps_ >= 3) { gridT_ += diff; jumps_ = 0; ++stats_.phaseJumps; }
        }
    }

    float lead_, dt_ = .01f, lockAt_ = .2f, releaseAt_ = .12f;
    double pulseOffset_ = 0;
    float memory_ = 0; double pendingDiff_ = 0;
    Stats stats_;  // last locked period, kept through breaks
    std::vector<float> x_, c_, raw_, score_;
    double t_ = 0, lastOnset_ = -1, gridT_ = 0;
    float hl_[3]{}, hh_[3]{}; int pos_ = 0, filled_ = 0;
    float mean_ = 0, dev_ = 0, prev_ = 0, prev2_ = 0, prevThreshold_ = 1e9f, strength_ = .6f;
    float history_[kHistory]{}; int head_ = 0, count_ = 0, sinceEstimate_ = 0;
    bool locked_ = false, matched_ = false, nextMatched_ = false; int misses_ = 0, jumps_ = 0, switchVotes_ = 0;
    float period_ = .5f;
};

}  // namespace nowplaying
