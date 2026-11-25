#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace docky::pairing {

constexpr int kPcPeerCount = 5;

struct PcBubble {
    float x{}, y{}, radius{}, opacity{};
};

struct PcLayout {
    PcBubble local;
    std::array<PcBubble, kPcPeerCount> peers;
};

inline PcLayout LayoutPcBubbles(float width, float height) {
    const float radius = std::min(width * 0.218f, height * 0.209f);
    PcLayout layout{{width * 0.5f, height * 0.35f, radius, 1.0f}, {}};
    const float step = std::min(width * 0.172f, height * 0.18f);
    const float peerRadius = std::min(step * 0.36f, radius * 0.30f);
    for (int slot = 0; slot < kPcPeerCount; ++slot)
        layout.peers[slot] = {layout.local.x + (slot - kPcPeerCount / 2) * step,
            height * 0.735f, peerRadius, 1.0f};
    return layout;
}

// Selection hides unselected peers. Confirming moves the selected peer into the main ball.
class PcPairingMotion {
public:
    void Discover(int slot, double now) {
        if (slot >= 0 && slot < kPcPeerCount && found_[slot] < 0.0) found_[slot] = now;
    }

    bool IsDiscovered(int slot) const {
        return slot >= 0 && slot < kPcPeerCount && found_[slot] >= 0.0;
    }

    bool Request(int slot, double now) {
        if (!IsDiscovered(slot) || slot == pending_ || connected_ >= 0) return false;
        CaptureVisibility(now);
        pending_ = slot;
        return true;
    }

    bool Confirm(int slot, double now) {
        if (slot < 0 || pending_ != slot) return false;
        CaptureVisibility(now);
        pending_ = -1;
        connected_ = slot;
        mergePeer_ = slot;
        mergeAt_ = now;
        mergeFrom_ = 0.0f;
        return true;
    }

    void Disconnect(double now) {
        if (pending_ < 0 && connected_ < 0) return;
        CaptureVisibility(now);
        if (connected_ >= 0) {
            mergeFrom_ = MergeAmount(now, false);
            mergeAt_ = now;
        }
        pending_ = -1;
        connected_ = -1;
    }

    int Pending() const { return pending_; }
    int Connected() const { return connected_; }
    float MergeAmount(double now, bool reduced) const {
        if (mergePeer_ < 0) return 0.0f;
        if (reduced) return connected_ >= 0 ? 1.0f : 0.0f;
        const float t = Unit(now - mergeAt_, connected_ >= 0 ? 0.36 : 0.24);
        const float eased = t * t * (3.0f - 2.0f * t);
        return connected_ >= 0 ? eased : mergeFrom_ * (1.0f - eased);
    }

    PcBubble Bubble(int slot, const PcLayout& layout, double now, bool reduced) const {
        if (!IsDiscovered(slot)) return {};
        const auto& target = layout.peers[slot];
        if (reduced) return connected_ < 0 && (pending_ < 0 || slot == pending_) ? target : PcBubble{};
        const float age = static_cast<float>(std::max(0.0, now - found_[slot]));
        const float phase = Phase(slot);
        const float speed = 0.84f + 0.35f * std::sin(phase * 2.1f);
        const float t = Unit(age, 0.43 + 0.09 * std::cos(phase));
        const float ease = 1.0f - std::pow(1.0f - t, 3.0f);
        const float drift = age * speed + phase;
        const float visibility = Visibility(slot, now);
        const float dx = target.radius * (0.11f * std::sin(drift) + 0.05f * std::sin(1.7f * drift));
        const float dy = target.radius * (0.17f * std::cos(drift * 1.3f) + 0.07f * std::sin(drift * 0.63f));
        PcBubble bubble{std::lerp(layout.local.x + (target.x - layout.local.x) * 0.75f, target.x + dx, ease),
            target.y + dy - target.radius * (0.82f * (1.0f - ease) - 0.35f * (1.0f - visibility)),
            target.radius * std::lerp(0.20f, 1.0f, ease) * (0.68f + 0.32f * visibility),
            std::min(1.0f, t * 2.25f) * visibility};
        if (slot == mergePeer_) {
            const float merge = MergeAmount(now, false);
            bubble.x = std::lerp(bubble.x, layout.local.x, merge);
            bubble.y = std::lerp(bubble.y, layout.local.y, merge);
            bubble.radius *= std::lerp(1.0f, 0.75f, merge);
            bubble.opacity *= 1.0f - Unit(merge - 0.68, 0.28);
        }
        return bubble;
    }

    float Phase(int slot) const {
        // Preserve each peer's phase throughout its lifetime, including reconnects.
        const double noise = std::sin(found_[slot] * 0.71 + (slot + 1) * 78.233) * 43758.5453;
        return static_cast<float>((noise - std::floor(noise)) * 6.2831853);
    }

private:
    float Visibility(int slot, double now) const {
        const int selected = connected_ >= 0 ? connected_ : pending_;
        const float goal = selected < 0 || slot == selected ? 1.0f : 0.0f;
        const float t = Unit(now - visibilityAt_, 0.20);
        return std::lerp(previousVisibility_[slot], goal, t * t * (3.0f - 2.0f * t));
    }
    void CaptureVisibility(double now) {
        for (int i = 0; i < kPcPeerCount; ++i) previousVisibility_[i] = Visibility(i, now);
        visibilityAt_ = now;
    }
    double visibilityAt_ = -1.0;
    std::array<float, kPcPeerCount> previousVisibility_{1, 1, 1, 1, 1};

    static float Unit(double elapsed, double duration) {
        return static_cast<float>(std::clamp(elapsed / duration, 0.0, 1.0));
    }

    std::array<double, kPcPeerCount> found_ = [] {
        std::array<double, kPcPeerCount> times{};
        times.fill(-1.0);
        return times;
    }();
    int pending_ = -1;
    int connected_ = -1;
    int mergePeer_ = -1;
    double mergeAt_ = 0.0;
    float mergeFrom_ = 0.0f;
};

}  // namespace docky::pairing
