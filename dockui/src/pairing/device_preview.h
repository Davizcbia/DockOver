#pragma once

#include <array>
#include <dwrite.h>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

#include "pairing_glass.h"
#include "pc_pairing_motion.h"
#include "phone_language.h"
#include "lan_picker.h"

namespace docky::pairing {

// The device picker simulates selection and sending. Add phone opens the existing pairing UI.
class DevicePreview {
public:
    static constexpr int kDrawnBubbles = 7;

    bool Init(ID3D11Device* device, int count, bool reducedMotion, void (*openPhone)()) {
        live_ = count < 0;
        count_ = live_ ? 0 : std::clamp(count, 1, static_cast<int>(devices_.size()));
        reducedMotion_ = reducedMotion;
        openPhone_ = openPhone;
        using Microsoft::WRL::ComPtr;
        ComPtr<IDWriteFactory> write;
        if (!glass_.Init(device) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(write.GetAddressOf())))) return false;
        return MakeFont(write.Get(), 23, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_) &&
            MakeFont(write.Get(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, label_) &&
            MakeFont(write.Get(), 12, DWRITE_FONT_WEIGHT_NORMAL, detail_);
    }

    void Resize() { ink_.Reset(); brush_.Reset(); }

    void Frame(UINT width, UINT height, float dpi, double now) {
        if (live_ && pressed_ == kNone && !dragging_ && lan_.Poll(now)) {
            count_ = static_cast<int>(lan_.peers.size());
            if (keyboard_ >= count_) keyboard_ = kPrimary;
        }
        scale_ = std::min({dpi, width / 610.0f, height / 680.0f});
        const float w = width / scale_, h = height / scale_;
        const bool initialFrame = width_ != w || height_ != h;
        width_ = w;
        height_ = h;
        const float x = width_ * 0.5f;
        const float deck = height_ - 252;
        frame_ = {2, 2, width_ - 2, height_ - 2};
        header_ = {24, 23, 158, 66};
        badge_ = {164, 27, 242, 62};
        close_ = {width_ - 65, 23, width_ - 24, 64};
        status_ = {x - 196, deck - 55, x + 196, deck - 13};
        rail_ = {x - 281, deck, x + 281, height_ - 77};
        viewport_ = {rail_.left + 10, rail_.top + 41, rail_.right - 10, rail_.bottom - 13};
        viewportPx_ = {static_cast<LONG>(std::ceil(viewport_.left * scale_)),
            static_cast<LONG>(std::ceil(viewport_.top * scale_)),
            static_cast<LONG>(std::floor(viewport_.right * scale_)),
            static_cast<LONG>(std::floor(viewport_.bottom * scale_))};
        main_ = {x, deck * 0.52f, 121, 1};
        left_ = {rail_.right - 88, rail_.top + 8, rail_.right - 54, rail_.top + 36};
        right_ = {rail_.right - 46, rail_.top + 8, rail_.right - 12, rail_.top + 36};
        const float buttonTop = height_ - 64;
        primary_ = {x - 103, buttonTop, x + 103, height_ - 22};
        secondary_ = {x - 174, buttonTop, x - 24, height_ - 22};
        pairedPrimary_ = {x - 13, buttonTop, x + 174, height_ - 22};
        if (live_) primary_ = pairedPrimary_;
        scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
        scrollTarget_ = std::clamp(scrollTarget_, 0.0f, MaxScroll());
        const float dt = lastFrame_ > 0 ? static_cast<float>(std::clamp(now - lastFrame_, 0.0, 0.05)) : 0;
        lastFrame_ = now;
        if (initialFrame || reducedMotion_) scroll_ = scrollTarget_;
        else if (!dragging_) scroll_ = std::lerp(scroll_, scrollTarget_, 1.0f - std::exp(-dt * 20.0f));
        if (phase_ == Phase::Sending && now - changedAt_ >= MergeDuration() + kTransferSeconds) {
            phase_ = Phase::Done;
            changedAt_ = now;
        }
    }

    PcBubble MainBall() const {
        PcBubble ball = Pixels(main_);
        const float merge = Merge();
        ball.radius *= 1.0f + 0.06f * std::sin(merge * 3.14159265f);
        return ball;
    }

    void Bubbles(PcBubble (&bubbles)[kDrawnBubbles]) const {
        if (phase_ == Phase::Done) return;
        if (phase_ != Phase::Browse) {
            if (selected_ >= 0 && Merge() < 1.0f) bubbles[0] = Pixels(SelectedBall());
            return;
        }
        const float returning = reducedMotion_ ? 1.0f : Ease((lastFrame_ - returnAt_) / 0.34);
        if (returning < 1 && returningDevice_ >= 0) {
            const auto end = DeviceBall(returningDevice_, Card(returningDevice_));
            bubbles[0] = Pixels({std::lerp(returnFrom_.x, end.x, returning), std::lerp(returnFrom_.y, end.y, returning),
                std::lerp(returnFrom_.radius, end.radius, returning), 1.0f});
        }
        int drawn = 1;
        for (int i = 0; i < count_ && drawn < kDrawnBubbles; ++i) {
            const auto card = Card(i);
            if (Visible(card) && (returning >= 1 || i != returningDevice_)) bubbles[drawn++] = Pixels(DeviceBall(i, card));
        }
    }

    const RECT& Viewport() const { return viewportPx_; }
    bool Browsing() const { return phase_ == Phase::Browse; }
    bool Done() const { return phase_ == Phase::Done; }
    bool Closed() const { return closed_; }
    void Reopen() { closed_ = false; }  // the same window shows the picker again (back from the phone page)
    double FinishedAt() const { return Done() ? changedAt_ : -100; }
    float MergeProgress() const { return Merge(); }
    float Progress() const {
        if (Done()) return 1.0f;
        return phase_ == Phase::Sending
            ? static_cast<float>(std::clamp((lastFrame_ - changedAt_ - MergeDuration()) / kTransferSeconds, 0.0, 1.0))
            : 0.0f;
    }

    bool PrepareBackdrop(UINT w, UINT h) { return glass_.Prepare(w, h); }

    void PaintGlass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target) {
        const float clear[4]{};
        ctx->ClearRenderTargetView(target, clear);
        auto panel = PairingGlass::PanelMaterial();
        auto control = PairingGlass::ControlMaterial(42.0f);
        const int active = Hit(pointer_);
        const auto plate = [&](D2D1_RECT_F rect, float radius, dock::GlassParameters& params, int action = kNone,
                               const RECT* clip = nullptr) {
            params.radius = radius;
            glass_.Draw(target, scale_, rect, params, pointer_.x * scale_, pointer_.y * scale_,
                action != kNone && active == action, action != kNone && pressed_ == action && !dragging_, clip);
        };
        plate(frame_, 37, panel);
        plate(header_, 19, control);
        if (!live_) plate(badge_, 17, control);
        plate(close_, 21, control, kClose);
        plate(status_, 19, control);
        if (Browsing()) {
            if (count_) plate(rail_, 23, control);
            if (MaxScroll() > 0) {
                plate(left_, 14, control, kLeft);
                plate(right_, 14, control, kRight);
            }
            for (int i = 0; i < count_; ++i) {
                const auto rect = Card(i);
                if (Visible(rect)) plate(rect, 18, control, i, &viewportPx_);
            }
        } else if (phase_ != Phase::Waiting) {
            plate(rail_, 23, control);
        }
        if (phase_ == Phase::Waiting) {
            plate(secondary_, 21, control, kSecondary);
            plate(pairedPrimary_, 21, control, kPrimary);
        } else {
            if (live_) plate(secondary_, 21, control, kSecondary);
            plate(primary_, 21, control, kPrimary);
        }
    }

    void PaintText(ID3D11Texture2D* back) {
        using Microsoft::WRL::ComPtr;
        if (!ink_) {
            ComPtr<IDXGISurface> surface;
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (FAILED(back->QueryInterface(IID_PPV_ARGS(&surface))) ||
                FAILED(factory_->CreateDxgiSurfaceRenderTarget(surface.Get(),
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, format, 96, 96), &ink_)) ||
                FAILED(ink_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_))) {
                Resize();
                return;
            }
            ink_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        ink_->BeginDraw();
        ink_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
        Text(L"DockDrop", header_, title_.Get());
        if (!live_) Text(T(L"界面演示", L"Preview"), badge_, detail_.Get(), true);
        Icon(close_, kClose);
        Text(Status(), status_, label_.Get());
        if (Browsing()) DrawDeviceRail();
        else DrawPhaseDetail();
        if (phase_ == Phase::Waiting) {
            Text(T(L"返回", L"Back"), secondary_, label_.Get());
            Text(T(L"模拟对方确认", L"Simulate acceptance"), pairedPrimary_, label_.Get());
        } else {
            const wchar_t* action = Browsing() ? T(L"配对手机", L"Pair phone")
                : Done() ? T(L"继续发送", L"Send again") : T(L"取消传输", L"Cancel transfer");
            if (live_) Text(T(L"接收电脑文件", L"Receive from PC"), secondary_, label_.Get());
            Text(action, primary_, label_.Get());
        }
        if (FAILED(ink_->EndDraw())) Resize();
    }

    bool InControls(POINT p) const {
        const auto dip = Dip(p);
        return Contains(rail_, dip) || Hit(dip) != kNone;
    }
    bool OnAction(POINT p) const { return Hit(Dip(p)) != kNone; }

    void Press(HWND window, POINT p) {
        pointer_ = Dip(p);
        pressed_ = Hit(pointer_);
        pressPoint_ = pointer_;
        scrollTarget_ = scroll_;
        startScroll_ = scroll_;
        dragCandidate_ = Browsing() && Contains(viewport_, pointer_) && MaxScroll() > 0;
        dragging_ = false;
        SetCapture(window);
    }

    void Move(POINT p) {
        pointer_ = Dip(p);
        if (!dragCandidate_) return;
        const float dx = pointer_.x - pressPoint_.x;
        if (!dragging_ && std::abs(dx) < 5.0f) return;
        dragging_ = true;
        scroll_ = scrollTarget_ = std::clamp(startScroll_ - dx, 0.0f, MaxScroll());
    }

    void Release(HWND window, POINT p, double now) {
        Move(p);
        const int action = Hit(pointer_);
        const bool invoke = !dragging_ && action != kNone && action == pressed_;
        CancelPress();
        if (GetCapture() == window) ReleaseCapture();
        if (invoke) Invoke(action, now);
    }

    void CancelPress() { pressed_ = kNone; dragging_ = dragCandidate_ = false; }

    void Wheel(float steps) {
        if (Browsing()) ScrollTo(scrollTarget_ + steps * kStep * 0.80f);
    }

    bool Key(WPARAM key, double now) {
        if (key == VK_ESCAPE) {
            if (Browsing()) closed_ = true;
            else ReturnToDevices(now);
            return true;
        }
        if (key == VK_TAB) {
            const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (Browsing()) {
                if (live_ && keyboard_ == kSecondary) keyboard_ = back && count_ ? count_-1 : kPrimary;
                else if (live_ && ((keyboard_ == kPrimary && back) || (!back && keyboard_ == count_-1) || (count_ == 0 && keyboard_ == kPrimary))) keyboard_ = kSecondary;
                else if (count_ == 0) keyboard_ = kPrimary;
                else if (keyboard_ == kPrimary) keyboard_ = back ? count_ - 1 : 0;
                else if (keyboard_ == kNone) keyboard_ = back ? kPrimary : 0;
                else if (back) keyboard_ = keyboard_ == 0 ? kPrimary : keyboard_ - 1;
                else keyboard_ = keyboard_ + 1 == count_ ? kPrimary : keyboard_ + 1;
                if (keyboard_ >= 0) EnsureVisible(keyboard_);
            } else {
                keyboard_ = phase_ == Phase::Waiting && keyboard_ == kPrimary
                    ? kSecondary : kPrimary;
            }
            return true;
        }
        if (Browsing() && count_ > 0 && (key == VK_LEFT || key == VK_RIGHT)) {
            keyboard_ = keyboard_ < 0 ? 0 : std::clamp(keyboard_ + (key == VK_LEFT ? -1 : 1), 0, count_ - 1);
            EnsureVisible(keyboard_);
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            if (keyboard_ != kNone) Invoke(keyboard_, now);
            return true;
        }
        return false;
    }

private:
    using ComBrush = Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>;
    enum class Phase { Browse, Waiting, Sending, Done };
    enum Action { kNone = -1, kClose = -2, kLeft = -3, kRight = -4, kPrimary = -5, kSecondary = -6 };
    struct Device { const wchar_t* name; bool phone; };
    static constexpr std::array devices_ = {
        Device{L"Studio PC", false}, Device{L"iPhone 16", true}, Device{L"ThinkPad", false},
        Device{L"Pixel 9", true}, Device{L"Surface", false}, Device{L"Desk Mini", false},
        Device{L"Galaxy S24", true}, Device{L"Workstation", false}, Device{L"Zenbook", false},
        Device{L"Pixel 8", true}, Device{L"Living Room", false}, Device{L"Surface Go", false},
        Device{L"iPhone 15", true}, Device{L"Travel PC", false}
    };
    static constexpr float kStep = 121.0f, kCardWidth = 110.0f;
    static constexpr double kTransferSeconds = 2.6;

    static const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
        return PRIMARYLANGID(uiLanguage) == LANG_CHINESE ? zh : en;
    }

    static bool MakeFont(IDWriteFactory* factory, float size, DWRITE_FONT_WEIGHT weight,
                         Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"", out.GetAddressOf()))) return false;
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    static bool Contains(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }
    D2D1_POINT_2F Dip(POINT p) const { return {p.x / scale_, p.y / scale_}; }
    PcBubble Pixels(PcBubble b) const { return {b.x * scale_, b.y * scale_, b.radius * scale_, b.opacity}; }
    float ContentWidth() const { return (count_ - 1) * kStep + kCardWidth; }
    float MaxScroll() const { return std::max(0.0f, ContentWidth() - (viewport_.right - viewport_.left)); }
    void ScrollTo(float value) { scrollTarget_ = std::clamp(value, 0.0f, MaxScroll()); }

    void EnsureVisible(int index) {
        const float left = index * kStep;
        const float right = left + kCardWidth - (viewport_.right - viewport_.left);
        ScrollTo(std::clamp(scrollTarget_, std::min(left, right), left));
    }

    D2D1_RECT_F Card(int i) const {
        const float inset = std::max(0.0f, (viewport_.right - viewport_.left - ContentWidth()) * 0.5f);
        const float left = viewport_.left + inset + i * kStep - scroll_;
        return {left, viewport_.top + 2, left + kCardWidth, viewport_.bottom - 2};
    }
    bool Visible(const D2D1_RECT_F& r) const { return r.right > viewport_.left && r.left < viewport_.right; }

    PcBubble DeviceBall(int i, const D2D1_RECT_F& card) const {
        const float wave = reducedMotion_ ? 0.0f : static_cast<float>(std::sin(lastFrame_ * (1.02 + i * 0.02) + i * 2.29));
        return {(card.left + card.right) * 0.5f + wave * 1.8f, card.top + 34 + wave * 2.4f, 27.0f, 1};
    }

    double MergeDuration() const { return reducedMotion_ ? 0.0 : 0.64; }
    static float Ease(double v) {
        const float t = static_cast<float>(std::clamp(v, 0.0, 1.0));
        return t * t * (3 - 2 * t);
    }
    float Merge() const {
        if (Done()) return 1;
        if (phase_ != Phase::Sending) return 0;
        return reducedMotion_ ? 1 : Ease((lastFrame_ - changedAt_) / MergeDuration());
    }
    PcBubble SelectedBall() const {
        const PcBubble ready{main_.x + main_.radius + 52, main_.y + 30, 36, 1};
        const float approach = reducedMotion_ ? 1 : Ease((lastFrame_ - selectedAt_) / 0.40);
        if (phase_ == Phase::Waiting) return {std::lerp(selectedFrom_.x, ready.x, approach),
            std::lerp(selectedFrom_.y, ready.y, approach), std::lerp(selectedFrom_.radius, ready.radius, approach), 1};
        const float merge = Merge();
        const float blur = 1.0f - Ease((merge - 0.75f) / 0.25f);
        return {std::lerp(mergeFrom_.x, main_.x, merge), std::lerp(mergeFrom_.y, main_.y, merge),
            mergeFrom_.radius, blur};
    }

    int Hit(D2D1_POINT_2F p) const {
        if (Contains(close_, p)) return kClose;
        if (live_ && Contains(secondary_,p)) return kSecondary;
        if (Browsing()) {
            if (MaxScroll() > 0 && Contains(left_, p)) return kLeft;
            if (MaxScroll() > 0 && Contains(right_, p)) return kRight;
            if (Contains(viewport_, p)) {
                for (int i = 0; i < count_; ++i) if (Contains(Card(i), p)) return i;
            }
        }
        if (phase_ == Phase::Waiting) {
            if (Contains(secondary_, p)) return kSecondary;
            if (Contains(pairedPrimary_, p)) return kPrimary;
        } else if (Contains(primary_, p)) return kPrimary;
        return kNone;
    }

    void ReturnToDevices(double now) {
        if (phase_ == Phase::Waiting) {
            returnAt_ = now;
            returningDevice_ = selected_;
            returnFrom_ = SelectedBall();
        }
        phase_ = Phase::Browse;
        keyboard_ = selected_;
        selected_ = -1;
    }

    void Invoke(int action, double now) {
        keyboard_ = action == kLeft || action == kRight ? kNone : action;
        if (action >= 0 && action < count_ && Browsing()) {
            if (live_) {
                if (!lan_.Send(lan_.peers[action].id)) MessageBoxW(GetActiveWindow(), T(L"无法启动电脑传输组件", L"Could not start PC transfer"), L"DockDrop", MB_ICONERROR);
                return;
            }
            selectedFrom_ = DeviceBall(action, Card(action));
            selected_ = action;
            selectedAt_ = now;
            phase_ = Phase::Waiting;
            keyboard_ = kPrimary;
        } else if (action == kClose) closed_ = true;
        else if (action == kLeft || action == kRight) Wheel(action == kLeft ? -3.0f : 3.0f);
        else if (action == kSecondary && live_) {
            if (!lan_.Receive()) MessageBoxW(GetActiveWindow(), T(L"无法启动电脑接收",L"Could not start receiver"), L"DockDrop", MB_ICONERROR);
        } else if (action == kSecondary) ReturnToDevices(now);
        else if (action == kPrimary) {
            switch (phase_) {
            case Phase::Browse:
                openPhone_();
                break;
            case Phase::Waiting:
                mergeFrom_ = SelectedBall();
                phase_ = Phase::Sending;
                changedAt_ = now;
                break;
            case Phase::Sending:
            case Phase::Done:
                ReturnToDevices(now);
                break;
            }
        }
    }

    std::wstring Status() const {
        switch (phase_) {
        case Phase::Browse:
            if (live_ && lan_.Failed()) return T(L"电脑发现组件不可用", L"PC discovery unavailable");
            if (live_ && !count_) return T(L"正在寻找附近的电脑…", L"Looking for nearby PCs…");
            return T(L"选择接收设备", L"Choose a device");
        case Phase::Waiting: return std::wstring(T(L"等待确认 · ", L"Waiting for ")) + devices_[selected_].name;
        case Phase::Sending: return std::wstring(T(L"正在发送给 ", L"Sending to ")) + devices_[selected_].name;
        case Phase::Done: return std::wstring(T(L"已发送给 ", L"Sent to ")) + devices_[selected_].name;
        }
        return {};
    }

    void Text(std::wstring_view text, D2D1_RECT_F rect, IDWriteTextFormat* format, bool muted = false) {
        brush_->SetColor(muted ? D2D1::ColorF(0.73f, 0.81f, 0.89f) : D2D1::ColorF(0.94f, 0.97f, 1.0f));
        ink_->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Outline(D2D1_RECT_F rect, float radius) {
        brush_->SetColor(D2D1::ColorF(0.49f, 0.78f, 1.0f, 0.84f));
        ink_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), 1.25f);
    }

    void Icon(D2D1_RECT_F rect, int action, float opacity = 1.0f) {
        const float x = (rect.left + rect.right) * 0.5f, y = (rect.top + rect.bottom) * 0.5f;
        brush_->SetColor(D2D1::ColorF(0.91f, 0.96f, 1.0f, opacity));
        const auto line = [&](float ax, float ay, float bx, float by) {
            ink_->DrawLine({x + ax, y + ay}, {x + bx, y + by}, brush_.Get(), 1.5f);
        };
        if (action == kClose) {
            line(-4, -4, 4, 4);
            line(4, -4, -4, 4);
        } else {
            const float dir = action == kLeft ? 1.0f : -1.0f;
            line(2 * dir, -4, -2 * dir, 0);
            line(-2 * dir, 0, 2 * dir, 4);
        }
    }

    void DeviceIcon(bool phone, float x, float y) {
        brush_->SetColor(D2D1::ColorF(0.76f, 0.87f, 0.96f));
        if (phone) {
            ink_->DrawRoundedRectangle(D2D1::RoundedRect({x - 3.8f, y - 6, x + 3.8f, y + 6}, 1.7f, 1.7f), brush_.Get(), 1.0f);
            ink_->DrawLine({x - 1.4f, y + 4}, {x + 1.4f, y + 4}, brush_.Get(), 0.8f);
        } else {
            ink_->DrawRoundedRectangle(D2D1::RoundedRect({x - 6.0f, y - 4.8f, x + 6.0f, y + 2.6f}, 1.2f, 1.2f), brush_.Get(), 1.0f);
            ink_->DrawLine({x, y + 2.6f}, {x, y + 4.7f}, brush_.Get(), 1.0f);
            ink_->DrawLine({x - 3, y + 4.7f}, {x + 3, y + 4.7f}, brush_.Get(), 1.0f);
        }
    }

    void DrawDeviceRail() {
        if (live_ && !count_) return;
        const auto count = std::wstring(T(L"设备 · ", L"Devices · ")) + std::to_wstring(count_);
        Text(count, {rail_.left + 8, rail_.top + 6, rail_.left + 124, rail_.top + 37}, label_.Get());
        if (MaxScroll() > 0) {
            Icon(left_, kLeft, scroll_ > 1 ? 1.0f : 0.35f);
            Icon(right_, kRight, scroll_ < MaxScroll() - 1 ? 1.0f : 0.35f);
            const float track = 92, x = width_ * 0.5f - track * 0.5f, y = rail_.bottom - 7;
            brush_->SetColor(D2D1::ColorF(0.75f, 0.86f, 1.0f, 0.19f));
            ink_->DrawLine({x, y}, {x + track, y}, brush_.Get(), 2.0f);
            const float thumb = track * (viewport_.right - viewport_.left) / ContentWidth();
            const float pos = x + (track - thumb) * scroll_ / MaxScroll();
            brush_->SetColor(D2D1::ColorF(0.75f, 0.88f, 1.0f, 0.75f));
            ink_->DrawLine({pos, y}, {pos + thumb, y}, brush_.Get(), 2.0f);
        }
        ink_->PushAxisAlignedClip(viewport_, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < count_; ++i) {
            const auto rect = Card(i);
            if (!Visible(rect)) continue;
            const Device item = live_ ? Device{lan_.peers[i].name.c_str(), false} : devices_[i];
            Text(item.name, {rect.left + 3, rect.top + 65, rect.right - 3, rect.top + 87}, label_.Get());
            DeviceIcon(item.phone, rect.left + 22, rect.top + 96);
            Text(item.phone ? T(L"手机", L"Phone") : T(L"电脑", L"PC"),
                {rect.left + 31, rect.top + 84, rect.right - 8, rect.top + 107}, detail_.Get(), true);
            if (keyboard_ == i) Outline({rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2}, 16);
        }
        ink_->PopAxisAlignedClip();
        if (keyboard_ == kPrimary) Outline(primary_, 21);
    }

    void DrawPhaseDetail() {
        const D2D1_RECT_F copy{rail_.left + 18, rail_.top + 16, rail_.right - 18, rail_.bottom - 17};
        if (phase_ == Phase::Sending) Text(std::to_wstring(static_cast<int>(Progress() * 100)) + L"%", copy, title_.Get());
        else if (Done()) Text(T(L"传输完成", L"Transfer complete"), copy, title_.Get());
        if (keyboard_ == kPrimary) Outline(phase_ == Phase::Waiting ? pairedPrimary_ : primary_, 21);
        if (keyboard_ == kSecondary) Outline(secondary_, 21);
    }

    LanPicker lan_;
    bool live_ = false;
    Phase phase_ = Phase::Browse;
    int count_ = 0, selected_ = -1, keyboard_ = kNone, pressed_ = kNone, returningDevice_ = -1;
    bool reducedMotion_ = false, closed_ = false, dragCandidate_ = false, dragging_ = false;
    float width_ = 0, height_ = 0, scale_ = 1, scroll_ = 0, scrollTarget_ = 0, startScroll_ = 0;
    double lastFrame_ = 0, changedAt_ = 0, selectedAt_ = 0, returnAt_ = -100;
    D2D1_POINT_2F pointer_{-1, -1}, pressPoint_{};
    D2D1_RECT_F frame_{}, header_{}, badge_{}, close_{}, status_{}, rail_{}, viewport_{},
        left_{}, right_{}, primary_{}, secondary_{}, pairedPrimary_{};
    RECT viewportPx_{};
    PcBubble main_{}, selectedFrom_{}, mergeFrom_{}, returnFrom_{};
    void (*openPhone_)() = nullptr;
    PairingGlass glass_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> ink_;
    ComBrush brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_, label_, detail_;
};

}  // namespace docky::pairing
