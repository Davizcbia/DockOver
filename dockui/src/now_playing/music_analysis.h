#pragma once

#include "audio_beat.h"
#include <numbers>

namespace nowplaying {

// Keep the mid signal for normal material and move toward the louder channel
// when the stereo sum would otherwise cancel. Also works for mono.
class StereoFold {
public:
    float Push(float left, float right) {
        const float mid = (left + right) * .5f, side = (left - right) * .5f;
        midPower_ += .002f * (mid * mid - midPower_);
        sidePower_ += .002f * (side * side - sidePower_);
        const float mix = std::clamp((sidePower_ - 2 * midPower_) /
            std::max(1e-9f, sidePower_ + midPower_), 0.f, 1.f);
        blend_ += .002f * (mix - blend_);
        return mid + blend_ * side;
    }
private:
    float midPower_ = 0, sidePower_ = 0, blend_ = 0;
};

class MusicSpectrum {
public:
    static constexpr size_t kSize = 1024;
    void Configure(unsigned rate) {
        constexpr std::array edges{25.f, 55.f, 150.f, 300.f, 2000.f, 5000.f, 14000.f};
        weights_ = {};
        weightSum_ = {};
        for (size_t i = 0, reversed = 0; i < kSize; ++i) {
            reversed_[i] = reversed;
            for (size_t bit = kSize / 2; bit; bit /= 2) {
                reversed ^= bit;
                if (reversed & bit) break;
            }
            const float angle = 2 * std::numbers::pi_v<float> * float(i) / kSize;
            window_[i] = .5f - .5f * std::cos(angle);
            if (i >= kSize / 2) continue;
            cos_[i] = std::cos(angle); sin_[i] = -std::sin(angle);
            if (!i) continue;
            const float spacing = float(rate) / kSize, hz = i * spacing;
            for (size_t b = 0; b < weights_.size(); ++b) {
                const float overlap = std::min(hz + spacing / 2, edges[b + 1]) -
                    std::max(hz - spacing / 2, edges[b]);
                const float w = std::max(0.f, overlap / spacing);
                weights_[b][i] = w;
                weightSum_[b] += w;
            }
        }
        Reset();
    }
    void Reset() {
        cursor_ = filled_ = 0;
        previous_.fill(0); previousPower_.fill(0); floor_.fill(0);
        ready_ = false;
    }
    void Push(float sample) {
        samples_[cursor_] = sample;
        cursor_ = (cursor_ + 1) & (kSize - 1);
        filled_ = std::min(filled_ + 1, kSize);
    }
    struct Change { std::array<float, 6> flux{}, rise{}, power{}; float upperSpread = 0; };
    bool Ready() const { return ready_; }
    Change Analyze() {
        Change change;
        if (filled_ < kSize) return change;
        for (size_t i = 0; i < kSize; ++i) {
            real_[reversed_[i]] = samples_[(cursor_ + i) & (kSize - 1)] * window_[i];
        }
        imag_.fill(0);
        for (size_t width = 2; width <= kSize; width *= 2) {
            for (size_t start = 0; start < kSize; start += width) {
                for (size_t j = 0; j < width / 2; ++j) {
                    const size_t a = start + j, b = a + width / 2, w = j * (kSize / width);
                    const float r = real_[b] * cos_[w] - imag_[b] * sin_[w];
                    const float im = real_[b] * sin_[w] + imag_[b] * cos_[w];
                    real_[b] = real_[a] - r; imag_[b] = imag_[a] - im;
                    real_[a] += r; imag_[a] += im;
                }
            }
        }
        float upper = 0, upperPeak = 0;
        for (size_t i = 1; i < kSize / 2; ++i) {
            const float magnitude = std::sqrt(real_[i] * real_[i] + imag_[i] * imag_[i]) * (4.f / kSize);
            const float excess = std::max(0.f, magnitude - floor_[i]);
            // Bound whitening gain so a nearly empty bin cannot dominate the flux.
            const float value = std::log1p(excess / std::max(.00008f, 1.5f * floor_[i]));
            const float delta = ready_ ? std::max(0.f, value - previous_[i]) : 0.f;
            const float power = magnitude * magnitude;
            const float rise = ready_ ? std::max(0.f, power - previousPower_[i]) : 0.f;
            if (weights_[4][i] || weights_[5][i]) { upper += rise; upperPeak = std::max(upperPeak, rise); }
            previousPower_[i] = power;
            floor_[i] += (magnitude - floor_[i]) * (magnitude > floor_[i] ? .004f : .07f);
            const float weight = std::min(1.f, magnitude * 25000.f);
            for (size_t b = 0; b < change.flux.size(); ++b) {
                change.flux[b] += weights_[b][i] * delta * weight;
                change.rise[b] += weights_[b][i] * rise;
                change.power[b] += weights_[b][i] * power;
            }
            // Store the previous magnitude on the updated noise scale as well.
            previous_[i] = std::log1p(std::max(0.f, magnitude - floor_[i]) /
                std::max(.00008f, 1.5f * floor_[i]));
        }
        ready_ = true;
        change.upperSpread = upper / std::max(1e-12f, upperPeak);
        for (size_t b = 0; b < change.flux.size(); ++b) change.flux[b] /= std::max(1.f, weightSum_[b]);
        return change;
    }
private:
    size_t cursor_ = 0, filled_ = 0;
    bool ready_ = false;
    std::array<size_t, kSize> reversed_{};
    std::array<float, kSize> samples_{}, window_{}, real_{}, imag_{};
    std::array<float, kSize / 2> cos_{}, sin_{}, previous_{}, previousPower_{}, floor_{};
    std::array<std::array<float, kSize / 2>, 6> weights_{};
    std::array<float, 6> weightSum_{};
};

class MusicStructure {
public:
    void Feed(MusicFrame& frame, float dt, float power, float bassPower, bool onset, bool silent) {
        const float db = 10.f * std::log10(std::max(1e-12f, power));
        const float bassShare = bassPower / std::max(1e-12f, power);
        fast_ += dt / .25f * (db - fast_);
        slow_ += dt / 5.f * (db - slow_);
        bassFast_ += dt / .2f * (bassShare - bassFast_);
        bassSlow_ += dt / 4.f * (bassShare - bassSlow_);
        density_ += dt * ((onset ? 1.f / dt : 0.f) - density_);
        densitySlow_ += dt / 6.f * (density_ - densitySlow_);
        ceiling_ = std::max(fast_, ceiling_ - dt * .6f);
        cooldown_ = std::max(0.f, cooldown_ - dt);
        const bool gainStep = age_ > 1 && std::abs(db - previous_) > 5.f &&
            std::abs(bassShare - previousBass_) < .025f;
        previous_ = db;
        previousBass_ = bassShare;
        if (gainStep) { gainGuard_ = .45f; arm_ = build_ = 0; }
        gainGuard_ = std::max(0.f, gainGuard_ - dt);
        silence_ = silent ? silence_ + dt : 0.f;
        if (silence_ > .65f) {
            *this = MusicStructure{};
            return;
        }
        age_ += dt;
        if (age_ < 2.f || silent) return;
        sinceOnset_ = onset ? 0.f : sinceOnset_ + dt;
        const float trend = fast_ - slow_;
        const bool reducedBass = bassSlow_ > .025f && bassFast_ < bassSlow_ * .6f;
        const bool risingDensity = density_ > std::max(1.5f, densitySlow_ + .65f);
        const bool building = reducedBass && (risingDensity || trend > 2.f) && gainGuard_ <= 0;
        build_ = std::clamp(build_ + (building ? dt : -2 * dt), 0.f, 1.2f);
        if (building && build_ > .55f) {
            if (arm_ <= 0) valley_ = fast_;
            arm_ = 3.f;
        }
        if (arm_ > 0) {
            arm_ -= dt;
            valley_ = std::min(valley_, fast_);
            if (cooldown_ <= 0 && gainGuard_ <= 0 && fast_ > valley_ + 4.f && fast_ > ceiling_ - 3.f &&
                bassFast_ > std::max(.035f, .85f * bassSlow_) && sinceOnset_ < .18f) {
                frame.Add(RhythmKind::Drop, frame.at, 1.f, .75f);
                arm_ = build_ = 0;
                cooldown_ = 8.f;
            }
        }
        if (arm_ > 0 || building) frame.state = MusicState::Building;
        else if (trend > 2.f && density_ > 1.f) frame.state = MusicState::Peak;
        else if (trend < -3.f) frame.state = MusicState::Decaying;
        else if (fast_ < ceiling_ - 8.f && density_ < 1.f) frame.state = MusicState::Quiet;
        else frame.state = MusicState::Steady;
        frame.structureConfidence = std::clamp((age_ - 2.f) / 4.f, 0.f, .85f);
    }
private:
    float fast_ = -80, slow_ = -80, ceiling_ = -80, previous_ = -80;
    float bassFast_ = 0, bassSlow_ = 0, previousBass_ = 0;
    float density_ = 0, densitySlow_ = 0, build_ = 0, arm_ = 0, valley_ = 0;
    float age_ = 0, silence_ = 0, cooldown_ = 0, gainGuard_ = 0, sinceOnset_ = 1;
};

class MusicAnalyzer {
public:
    void Configure(unsigned rate, float lead = .02f) {
        rate_ = rate;
        lead_ = lead;
        spectrum_.Configure(rate);
        Reset();
    }
    void Reset() {
        bands_.Reset(rate_);
        spectrum_.Reset();
        tracker_ = BeatTracker(lead_);
        tracker_.EnableMusicTracking();
        structure_ = {};
        detectors_ = {};
        barAccents_ = {};
        bars_ = gridRevision_ = barOffset_ = 0;
        envelopes_ = {};
        lastBeatAt_ = -1e9;
        barConfidence_ = level_ = lowFast_ = lowSlow_ = power_ = peak_ = accent_ = kickDensity_ = previousLowDb_ = 0;
        previousAttack_ = 0;
        percussion_ = recentKickSupport_ = previousSnareSupport_ = 0;
        percussionReady_ = false;
        accentIndex_ = 0;
    }
    void SetLockThresholds(float lock, float release) { tracker_.SetLockThresholds(lock, release); }
    void SetSensitivity(int value) { sensitivity_ = (std::clamp(value, 0, 100) - 50) / 50.f; }
    bool Push(float sample, double at, MusicFrame& frame) {
        spectrum_.Push(sample);
        power_ += sample * sample;
        float low, high;
        if (!bands_.Push(sample, low, high)) return false;
        const float dt = bands_.BlockSeconds();
        const float power = power_ / std::max(1u, rate_ / 100);
        power_ = 0;
        frame = {};
        frame.at = at;
        const bool silent = power < 1e-9f;
        peak_ = std::max(power, peak_ * .9975f);
        const float target = silent ? 0.f : std::sqrt(power / std::max(peak_, 1e-9f));
        level_ += (target - level_) * (target > level_ ? .55f : .13f);
        frame.level = level_;
        frame.bass = silent ? 0.f : std::clamp(4.f * std::sqrt(low / std::max(peak_, 1e-9f)), 0.f, 1.f);
        const float lowDb = std::log1p(low * 40000.f);
        lowFast_ += .65f * (lowDb - lowFast_);
        lowSlow_ += .065f * (lowDb - lowSlow_);
        const float lowRise = std::max(0.f, lowFast_ - lowSlow_);
        const float lowAttack = std::max(0.f, lowFast_ - previousLowDb_);
        previousLowDb_ = lowFast_;
        const auto change = spectrum_.Analyze();
        for (size_t i = 0; i < frame.bands.size(); ++i) {
            const float energy = silent ? 0.f : std::clamp(std::sqrt(change.power[i] / std::max(peak_, 1e-9f)), 0.f, 1.f);
            frame.bands[i] = envelopes_[i].Step(energy, dt, .014f, .11f);
        }
        const auto& flux = change.flux;
        const auto& rise = change.rise;
        const float upper = rise[3] + rise[4] + rise[5];
        const float totalRise = std::max(1e-12f, rise[0] + rise[1] + rise[2] + upper);
        const float kick = .70f * lowRise + .70f * flux[1] + .12f * flux[2];
        const float snareBody = rise[2] / totalRise;
        const float snareSnap = (rise[3] + rise[4]) / totalRise;
        const float upperShare = (rise[4] + rise[5]) / totalRise;
        const float snare = (.45f * flux[2] + .65f * flux[4]) *
            std::clamp((snareBody - .035f) * 6.f, 0.f, 1.f) *
            std::clamp((snareSnap - .018f) * 10.f, 0.f, 1.f) *
            std::clamp((rise[3] + rise[4]) / std::max(1e-9f, power * .065f), 0.f, 1.f) *
            std::clamp((change.upperSpread - 3.5f) / 5.f, 0.f, 1.f) *
            std::clamp((upperShare - .008f) / .025f, 0.f, 1.f);
        const float hat = std::max(0.f, flux[5] - .45f * flux[4]) *
            std::clamp((rise[5] / std::max(1e-12f, upper) - .52f) * 4.f, 0.f, 1.f) *
            std::clamp((rise[5] / totalRise - .05f) * 6.f, 0.f, 1.f) *
            std::clamp(rise[5] / std::max(1e-9f, power * .025f), 0.f, 1.f);
        const float broadband = .20f * flux[2] + .37f * flux[3] + .28f * flux[4] + .15f * flux[5];
        // A sustained bass note can keep the slow envelope below the fast one.
        // Require fresh low-frequency evidence before classifying another attack.
        const float kickNovelty = lowRise > .035f ? kick : 0.f;
        const float attackEvidence = lowAttack + .35f * (flux[0] + flux[1]);
        const float kickSupport = std::clamp(((rise[0] + rise[1]) / totalRise - .18f) * 2.f, 0.f, 1.f);
        // The low-pass energy peak can trail the spectrum attack by a few hops.
        recentKickSupport_ = std::max(kickSupport, recentKickSupport_ * .72f);
        const float onset = std::max({.8f * kickNovelty, .85f * snare, broadband, .32f * hat});
        const std::array scores{onset, kickNovelty, snare, hat};
        const std::array gaps{.035, .085, .085, .035};
        const std::array floors{.075f, .12f, .15f, .095f};
        bool kickHit = false;
        float percussionHit = 0;
        const float thresholdScale = 1.f - .35f * sensitivity_;
        for (size_t i = 0; i < scores.size(); ++i) {
            auto& detector = detectors_[i];
            const float value = silent ? 0.f : scores[i];
            const float threshold = (detector.mean + 2.3f * detector.deviation + floors[i]) * thresholdScale;
            const bool isKick = i == size_t(RhythmKind::Kick);
            const double elapsed = at - dt - detector.last;
            // A strong kick leaves low-band ringing. A nearby kick is accepted
            // whenever its new attack clears this short, decaying floor.
            const float tailFloor = isKick && elapsed < .24 ?
                .65f * detector.lastStrength * std::exp(-float(elapsed) / .16f) : 0.f;
            const bool freshAttack = !isKick || (std::max(attackEvidence, previousAttack_) > .15f * thresholdScale &&
                (recentKickSupport_ > .12f * thresholdScale || !spectrum_.Ready()));
            if (!silent && freshAttack && detector.previous > std::max(detector.threshold, tailFloor) &&
                detector.previous >= detector.older && detector.previous > value && elapsed >= gaps[i]) {
                detector.last = at - dt;
                detector.lastStrength = detector.previous;
                const float confidence = std::clamp(.35f + .23f * (detector.previous / detector.threshold - 1), .35f, 1.f);
                frame.Add(static_cast<RhythmKind>(i), detector.last, std::min(1.f, detector.previous), confidence);
                kickHit |= isKick;
                if (isKick) percussionHit = std::max(percussionHit, confidence * recentKickSupport_);
                if (i == size_t(RhythmKind::Snare))
                    percussionHit = std::max(percussionHit, .8f * confidence * previousSnareSupport_);
            }
            detector.older = detector.previous;
            detector.previous = value;
            detector.threshold = threshold;
            const float residual = std::clamp(value - detector.mean, -.5f, .5f);
            detector.mean += dt * residual;
            detector.deviation += dt * (std::abs(residual) - detector.deviation);
        }
        previousAttack_ = attackEvidence;
        previousSnareSupport_ = change.upperSpread > 8.f ? std::clamp((upperShare - .015f) / .035f, 0.f, 1.f) : 0.f;
        const bool hit = frame.count && frame.events[0].kind == RhythmKind::Onset;
        kickDensity_ += dt * ((kickHit ? 1.f / dt : 0.f) - kickDensity_) / 1.5f;
        const float normalized = silent ? 0.f : std::max(0.f, onset - detectors_[0].mean) /
            std::max(.22f, .18f + 1.5f * detectors_[0].deviation);
        const float kickWeight = std::clamp((kickDensity_ - .75f) * .65f, 0.f, .85f);
        const float kickEvidence = kickHit ? 2.8f : .25f * std::min(2.f, kickNovelty);
        percussion_ = std::min(5.f, percussion_ * (1.f - dt / 2.f) + percussionHit);
        const float required = 1.5f - .34f * sensitivity_;
        percussionReady_ = percussion_ >= required * (percussionReady_ ? .68f : 1.f);
        const float pulse = tracker_.FeedFlux((1 - kickWeight) * std::min(normalized, 3.f) +
            kickWeight * kickEvidence, silent, dt, kickHit);
        if (percussionReady_) UpdateGrid(frame, pulse, std::sqrt(low));
        structure_.Feed(frame, dt, power, low, hit, silent);
        return true;
    }
private:
    void UpdateGrid(MusicFrame& frame, float pulse, float lowAmplitude) {
        frame.bpm = tracker_.Bpm();
        frame.period = tracker_.Period();
        frame.confidence = tracker_.Confidence();
        if (!tracker_.Locked() || gridRevision_ != tracker_.GridRevision()) {
            gridRevision_ = tracker_.GridRevision();
            barAccents_ = {};
            bars_ = barOffset_ = accentIndex_ = 0;
            accent_ = 0;
            barConfidence_ = 0;
        }
        if (!tracker_.Locked()) return;
        const auto index = tracker_.BeatIndex();
        const double anchor = frame.at + tracker_.GridOffset();
        frame.beatAnchor = anchor - (double(index) - barOffset_) * frame.period;
        const double sinceBeat = frame.at - anchor;
        if (sinceBeat >= 0 && sinceBeat < .095) accent_ = std::max(accent_, lowAmplitude);
        if (index != accentIndex_ && sinceBeat >= .095) {
            accentIndex_ = index;
            const auto slot = accentIndex_ % 4;
            barAccents_[slot] += .24f * (accent_ - barAccents_[slot]);
            accent_ = 0;
            if (slot == 0) {
                ++bars_;
                const auto strongest = std::max_element(barAccents_.begin(), barAccents_.end());
                float runner = 0;
                for (auto it = barAccents_.begin(); it != barAccents_.end(); ++it)
                    if (it != strongest) runner = std::max(runner, *it);
                const float margin = std::clamp(3.f * (*strongest - runner) / std::max(.01f, *strongest), 0.f, 1.f);
                const auto candidate = unsigned(strongest - barAccents_.begin());
                if (candidate != barOffset_) { barOffset_ = candidate; barConfidence_ = 0; }
                else if (bars_ >= 3) barConfidence_ += .45f * (margin - barConfidence_);
            }
        }
        frame.downbeatConfidence = barConfidence_ * frame.confidence;
        if (frame.downbeatConfidence < .38f) frame.beatAnchor = anchor - double(index) * frame.period;
        const double at = frame.at + tracker_.PulseOffset();
        if (pulse <= 0 || at <= lastBeatAt_ + .55 * frame.period) return;
        lastBeatAt_ = at;
        frame.Add(RhythmKind::Beat, at, pulse, frame.confidence);
        if (frame.downbeatConfidence >= .38f && index % 4 == barOffset_)
            frame.Add(RhythmKind::Downbeat, at, pulse, frame.downbeatConfidence);
    }
    struct Detector {
        float mean = 0, deviation = 0, previous = 0, older = 0, threshold = 1, lastStrength = 0;
        double last = -1e9;
    };
    unsigned rate_ = 48000;
    float lead_ = .02f;
    BandSplitter bands_;
    MusicSpectrum spectrum_;
    BeatTracker tracker_;
    MusicStructure structure_;
    std::array<Detector, 4> detectors_{};
    std::array<MusicEnvelope, 6> envelopes_{};
    std::array<float, 4> barAccents_{};
    unsigned bars_ = 0, gridRevision_ = 0, barOffset_ = 0, accentIndex_ = 0;
    double lastBeatAt_ = -1e9;
    float barConfidence_ = 0, level_ = 0, lowFast_ = 0, lowSlow_ = 0, power_ = 0, peak_ = 0, accent_ = 0, kickDensity_ = 0;
    float previousLowDb_ = 0;
    float previousAttack_ = 0;
    float sensitivity_ = 0, percussion_ = 0, recentKickSupport_ = 0, previousSnareSupport_ = 0;
    bool percussionReady_ = false;
};

}  // namespace nowplaying
