// docky-bubble: visual prototype of the Optical Bubble (Phase 2, the look only: no code, no pairing). A ball of liquid
// glass ray traced on the GPU (shaders/bubble.hlsl) in a borderless window, refracting what is behind it.
//
// Background (what the glass refracts):
//   default            the live desktop behind the window (DXGI desktop duplication, as Docky's glass; the window itself
//                      is excluded from capture, so the ball never sees itself)
//   --studio           a grey studio backdrop (light above, dark below) drawn in the window
//   --studio-dark      the same, darker: mid grey to dark grey
//   --image <file>     a picture (JPEG/PNG/BMP, via WIC) filling the window; --scroll <px/s> moves it sideways
// Other options:
//   --radius <dip>     ball radius (default 150);  --size <w> <h>  window size in DIP (default: 3.2 radii square)
//   --code-a           the code as light in the glass (Phase 3 candidate A); --code-b <amp>  as an invisible
//                      brightness watermark of that amplitude (candidate B); --code-rays <amp>  fused: faint beams of light from the
//                      centre, +-amp against the glass beside them (the scanner adds them up over frames); --cells <168 x 0/1>  the code's cells
//   --no-bubbles       no air bubbles inside;  --ripple <amp>  ripple height (default 0.014 radii)
//   --snapshot-seq <dir> --snapshot-after <s> --snapshot-interval <s> --snapshot-count <n>   BMP frames (opaque)
//   --exit-after <s>   quit after this many seconds;  --bench-out <file>  frame statistics on exit
//   --demo-pc [--demo-pc-tour]   preview the nearby PC and water route states without a network
// Keys: Esc quits. Drag anywhere to move the window.
#include "phone_language.h"
using docky::pairing::Ui;
#include <windows.h>
#include <windowsx.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../desktop/duplication.h"
#include "../vblank.h"
#include "bubble_ps.h"
#include "bubble_rays_ps.h"
#include "bubble_vs.h"
#include "pc_pairing_ps.h"
#include "pc_pairing_motion.h"
#ifdef DOCKY_PHONE_PAIRING
#include <commctrl.h>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include "../desktop_settings.h"
#include "../lab_slider.h"
#include "phone_host.h"
#include "trusted_phones.h"
#include "phone_settings_window.h"
#include "../json_lite.h"
#include "phone_transfer.h"
#include "widget_queue.h"
#include "pc_receive_prompt.h"
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kQrWordCount = 47 * 4;  // up to 77x77 modules, uint4 aligned
struct Constants {
    float targetSize[2], center[2];
    float radius, time, mode, opaque;
    float deskOrigin[2], deskSize[2];
    float imgScale[2], imgOffset[2];
    float dpi, showBubbles, rippleAmp, codeMode;
    float codeAmp, successAge, phonePanel, reducedMotion;
    uint32_t codeBits[8];
    float mouseImpacts[3][4], mouseFronts[4];
    float waterDrops[16][4], splashCount, sinkAge, finishAge, fileCount;
    uint32_t qrBits[kQrWordCount], qrSize;
    float rimProgress, rimOpacity, qrPad;
    docky::pairing::PcBubble pcPeers[docky::pairing::kPcPeerCount];
};
static_assert(sizeof(Constants) % 16 == 0, "constant buffer");

double Now() { return docky::vblank::Seconds(); }

struct Options {
    int mode = 0;  // 0 desktop, 1 image, 2 studio, 3 dark studio
    std::wstring image; double scroll = 0;
    float radius = 150, sizeW = 0, sizeH = 0, ripple = 0.014f;
    bool bubbles = true;
    int codeMode = 0; float codeAmp = 0.05f; uint32_t codeBits[8] = {};  // --code-a / --code-b <amp>, --cells <0/1 x 168>
    std::wstring seqDir; double seqAfter = 1.0, seqInterval = 0.1; int seqCount = 0;
    double exitAfter = 0; std::wstring benchOut;
    double demoProgress = 0;  // --demo-progress <seconds>: sweep the transfer rim once, for tuning and snapshots
    std::wstring demoSession;  // --demo-session <text, | for new lines>: paired, text received, transfer running; no phone
#ifdef DOCKDROP_STANDALONE
    bool pairPhone = true;
#else
    bool pairPhone = false;
#endif
    std::wstring settingsPath;
    bool registerDockDrop = false;
    bool pcPreview = false, pcTour = false, pcReduced = false;
} g_opt;

HWND g_wnd = nullptr;
HWND g_surface = nullptr;
bool g_quit = false;
double g_connectedAt = 0;
double g_sinkAt = -100, g_finishedAt = -100;
float g_transferProgress = 0.0f;
bool g_reduceMotion = false;

#ifdef DOCKY_PHONE_PAIRING
#ifdef DOCKDROP_STANDALONE
constexpr auto kPhoneWindow = L"DockDropPairing";
constexpr auto kPhoneMutex = L"Local\\DockDropPairingWindow";
#else
constexpr auto kPhoneWindow = L"DockyPhonePairing";
constexpr auto kPhoneMutex = L"Local\\DockOverPhonePairingWindow";
#endif
constexpr auto kScanLink = L"davizcbia.github.io/dockdrop-scan/bubble/?mode=rays";
constexpr UINT kQrToggle = 101, kClose = 103, kStatus = 104, kGuide = 105;
constexpr UINT kBallSize = 106, kBallSlider = 107;
constexpr UINT kReceiveSettings = 108, kCopyText = 109, kOpenFolder = 110;
constexpr UINT kSendFiles = 111, kSendText = 112;
constexpr UINT kReceivedText = 113, kBack = 114;
constexpr UINT kConfirmText = 115, kConfirmStay = 116, kConfirmLeave = 117;
constexpr UINT kPcStatus = 130, kPcDisconnect = 131, kPcFirstPeer = 132;
constexpr UINT_PTR kSaveRadiusTimer = 11;
constexpr COLORREF kControlText = RGB(237, 244, 255);
constexpr COLORREF kMutedText = RGB(196, 208, 220);

struct PhonePaintRuntime {
    ULONG_PTR token = 0;
    void Init() {
        Gdiplus::GdiplusStartupInput config;
        Gdiplus::GdiplusStartup(&token, &config, nullptr);
    }
    ~PhonePaintRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
HWND g_status = nullptr, g_guide = nullptr, g_qrButton = nullptr, g_close = nullptr, g_back = nullptr;
HWND g_sizeLabel = nullptr, g_slider = nullptr;
HWND g_receiveSettings = nullptr, g_receivedText = nullptr, g_copyText = nullptr, g_openFolder = nullptr;
HWND g_sendFiles = nullptr, g_sendText = nullptr;
HWND g_confirmText = nullptr, g_confirmStay = nullptr, g_confirmLeave = nullptr;
bool g_transferring = false, g_confirming = false, g_confirmClose = false;
bool g_hiddenForConfirm[3] = {};  // received text, copy, open folder
HBRUSH g_textBackdrop = nullptr;
ComPtr<ID2D1Factory> g_sliderFactory;
ComPtr<ID2D1DCRenderTarget> g_sliderTarget;
ComPtr<ID2D1SolidColorBrush> g_sliderBrush;
int g_preferredRadius = liquid::settings::kPhoneBallDefault, g_minRadius = 130, g_maxRadius = 252;
bool g_radiusDirty = false, g_sliderHeld = false;
float g_pressFrom = 0.0f, g_pressTo = 0.0f;
double g_pressAt = 0.0;
HFONT g_uiFont = nullptr;
int g_uiDpi = 0;
std::shared_ptr<docky::pairing::PhoneHost> g_phoneHost;
uint32_t g_qr[kQrWordCount]{};
int g_qrSize = 0;
bool g_showQr = false;
std::wstring g_receiveFolder, g_lastText, g_activePhone, g_targetPhone, g_shellSendPath;
constexpr ULONG_PTR kShellSendFile = 0x44444631;
constexpr UINT kShellSendReady = WM_APP + 42;
constexpr UINT kWidgetDraftReady = WM_APP + 43;
std::wstring g_receivePrompt;
bool g_offerSending = false;
constexpr UINT kPhoneOffer = WM_APP + 41;
docky::pairing::IncomingFile g_incoming;
docky::pairing::OutgoingTransfer g_outgoing;
docky::pairing::WidgetDraft g_widgetDraft;
bool g_manualFiles = false, g_manualText = false;
unsigned g_filesReceived = 0;

constexpr auto kPcWindow = L"DockDropPcPreview";
constexpr auto kPcReceiveBallWindow = L"DockDropPcReceiveBubble";
constexpr auto kPcSendBallWindow = L"DockDropPcSendBubble";
constexpr int kLivePeer = docky::pairing::kPcPeerCount / 2;
docky::pairing::pc_receive::Offer* g_pcOffer = nullptr;
constexpr const wchar_t* kPcNames[docky::pairing::kPcPeerCount]{
    L"Studio PC", L"ThinkPad", L"Desk Mini", L"Workstation", L"Surface"
};
docky::pairing::PcPairingMotion g_pcMotion;
HWND g_pcStatus = nullptr, g_pcDisconnect = nullptr;
HWND g_pcPeers[docky::pairing::kPcPeerCount]{};
double g_pcStarted = 0.0, g_pcConfirmAt = 0.0;
int g_pcTourStep = 0;

void LayoutPhoneControls(HWND window);
void ShowBackConfirm(bool show, bool closing = false);
void RevealAction(int slot);
void ResetPairing();
void PaintPhoneItem(const DRAWITEMSTRUCT& item);

docky::pairing::PcLayout PcLayoutFor(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    return docky::pairing::LayoutPcBubbles(static_cast<float>(rect.right), static_cast<float>(rect.bottom));
}

void LayoutPcControls(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    const UINT dpi = GetDpiForWindow(window);
    const auto dip = [dpi](int v) { return MulDiv(v, dpi, 96); };
    const auto layout = PcLayoutFor(window);
    SetWindowPos(g_surface, HWND_BOTTOM, 0, 0, rect.right, rect.bottom, SWP_NOACTIVATE);
    if (g_uiDpi != static_cast<int>(dpi)) {
        const HFONT font = CreateFontW(-dip(14), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        if (font) {
            if (g_uiFont) DeleteObject(g_uiFont);
            g_uiFont = font;
            g_uiDpi = dpi;
        }
    }
    const int statusWidth = std::min(dip(330), static_cast<int>(rect.right) / 2);
    MoveWindow(g_pcStatus, (rect.right - statusWidth) / 2, dip(19), statusWidth, dip(34), TRUE);
    MoveWindow(g_close, rect.right - dip(55), dip(19), dip(35), dip(35), TRUE);
    const int labelWidth = std::max(1, std::min(dip(101), static_cast<int>(layout.peers[1].x - layout.peers[0].x) - dip(5)));
    for (int i = 0; i < docky::pairing::kPcPeerCount; ++i) {
        const auto& bubble = layout.peers[i];
        MoveWindow(g_pcPeers[i], static_cast<int>(bubble.x) - labelWidth / 2,
            static_cast<int>(bubble.y + bubble.radius) + dip(10), labelWidth, dip(28), TRUE);
    }
    MoveWindow(g_pcDisconnect, (rect.right - dip(132)) / 2, rect.bottom - dip(48), dip(132), dip(31), TRUE);
}

void CreatePcControls(HWND window) {
    const auto create = [window](const wchar_t* type, const wchar_t* label, DWORD flags, UINT id) {
        return CreateWindowExW(0, type, label, WS_CHILD | WS_CLIPSIBLINGS | flags, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    };
    g_pcStatus = create(L"STATIC", Ui(L"正在发现附近的电脑…"), WS_VISIBLE | SS_OWNERDRAW, kPcStatus);
    for (int i = 0; i < docky::pairing::kPcPeerCount; ++i)
        g_pcPeers[i] = create(L"BUTTON", kPcNames[i], WS_TABSTOP | BS_OWNERDRAW, kPcFirstPeer + i);
    g_pcDisconnect = create(L"BUTTON", Ui(L"断开连接"), WS_TABSTOP | BS_OWNERDRAW, kPcDisconnect);
    g_close = create(L"BUTTON", Ui(L"关闭"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kClose);
    BOOL animation = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    g_reduceMotion = g_opt.pcReduced || !animation;
    LayoutPcControls(window);
}

void UpdatePcStatus() {
    if (!g_pcStatus) return;
    const int connected = g_pcMotion.Connected(), pending = g_pcMotion.Pending();
    std::wstring text;
    if (connected >= 0) text = std::wstring(Ui(g_finishedAt >= 0 ? L"已发送 · " : L"正在发送 · ")) + kPcNames[connected];
    else if (pending >= 0) text = std::wstring(Ui(L"等待对方确认 · ")) + kPcNames[pending];
    else text = Ui(L"附近的电脑");
    SetWindowTextW(g_pcStatus, text.c_str());
    SetWindowTextW(g_pcDisconnect, pending >= 0 ? Ui(L"取消连接") : Ui(L"断开连接"));

    for (int i = 0; i < docky::pairing::kPcPeerCount; ++i)
        ShowWindow(g_pcPeers[i], g_pcMotion.IsDiscovered(i) && connected < 0 &&
            (pending < 0 || pending == i) ? SW_SHOW : SW_HIDE);
    if (pending < 0 && connected < 0 && GetFocus() == g_pcDisconnect) SetFocus(g_pcPeers[0]);
    ShowWindow(g_pcDisconnect, pending >= 0 || connected >= 0 ? SW_SHOW : SW_HIDE);
}

void RequestDemoConnection(int slot, double now) {
    if (!g_pcMotion.Request(slot, now)) return;
    g_pcConfirmAt = now + 0.42;
    UpdatePcStatus();
}

void DisconnectDemoPeer(double now) {
    g_pcMotion.Disconnect(now);
    g_transferProgress = 0.0f;
    g_finishedAt = -100;
    UpdatePcStatus();
}

void TickPcPreview(double now) {
    const double age = now - g_pcStarted;
    for (int i = 0; i < docky::pairing::kPcPeerCount; ++i) {
        const double reveal = 0.24 + i * 0.10;
        if (age < reveal || g_pcMotion.IsDiscovered(i)) continue;
        g_pcMotion.Discover(i, g_pcStarted + reveal);
        UpdatePcStatus();
    }
    if (g_opt.pcTour) {
        if (g_pcTourStep == 0 && age >= 1.28) {
            RequestDemoConnection(3, now);
            ++g_pcTourStep;
        } else if (g_pcTourStep == 1 && age >= 3.70) {
            DisconnectDemoPeer(now);
            ++g_pcTourStep;
        } else if (g_pcTourStep == 2 && age >= 4.30) {
            RequestDemoConnection(1, now);
            ++g_pcTourStep;
        }
    }
    const int pending = g_pcMotion.Pending();
    if (pending >= 0 && now >= g_pcConfirmAt && g_pcMotion.Confirm(pending, now)) UpdatePcStatus();
    if (g_pcMotion.Connected() >= 0 && g_pcMotion.MergeAmount(now, g_reduceMotion) >= 1.0f) {
        g_transferProgress = static_cast<float>(std::clamp((now - g_pcConfirmAt - 0.36) / 1.25, 0.0, 1.0));
        if (g_transferProgress >= 1 && g_finishedAt < 0) { g_finishedAt = now; UpdatePcStatus(); }
    }
}

int PcPeerAt(POINT point, double now) {
    if (g_pcMotion.Connected() >= 0) return -1;
    const auto layout = PcLayoutFor(g_wnd);
    for (int i = 0; i < docky::pairing::kPcPeerCount; ++i) {
        if (g_pcMotion.Pending() >= 0 && i != g_pcMotion.Pending()) continue;
        const auto bubble = g_pcMotion.Bubble(i, layout, now, g_reduceMotion);
        if (bubble.opacity > 0.1f && std::hypot(point.x - bubble.x, point.y - bubble.y) <= bubble.radius * 1.08f)
            return i;
    }
    return -1;
}

void PollPcBall(double now) {
    auto& offer = *g_pcOffer;
    const auto tick = GetTickCount64();
    docky::pairing::pc_receive::PollInput(g_wnd, offer, tick);
    if (offer.Terminal() && tick >= offer.closeAt) { g_quit = true; return; }
    if (offer.sending && offer.accepted && g_pcMotion.Connected() < 0 && !offer.failed)
        g_pcMotion.Confirm(kLivePeer, now);
    if (offer.failed) {
        g_transferProgress = 0;
        return;
    }
    if (offer.sending && g_pcMotion.MergeAmount(now, g_reduceMotion) < 1) return;
    g_transferProgress = offer.complete ? 1.0f : std::min(offer.percent / 100.0f, 0.97f);
    if (offer.complete && g_finishedAt < 0) g_finishedAt = now;
}

bool IsPcReceiveBall() { return g_pcOffer && !g_pcOffer->sending; }

constexpr int kPcReceiveWindowDip = 320;
constexpr float kPcReceiveRadiusRatio = 0.438f;
enum class PcReceiveAction { None, Confirm, Close, OpenFolder };
PcReceiveAction g_pcReceivePressed = PcReceiveAction::None;
POINT g_pcReceivePressPoint{};
bool g_pcReceiveCloseVisible = false;

D2D1_RECT_F PcReceiveControl(PcReceiveAction action, float x, float y, float radius) {
    if (action == PcReceiveAction::Close)
        return D2D1::RectF(x + 0.403f * radius, y - 0.755f * radius, x + 0.647f * radius, y - 0.511f * radius);
    if (action == PcReceiveAction::OpenFolder)
        return D2D1::RectF(x - 0.56f * radius, y + 0.30f * radius, x + 0.56f * radius, y + 0.71f * radius);
    return D2D1::RectF(x - 0.47f * radius, y + 0.285f * radius, x + 0.47f * radius, y + 0.605f * radius);
}

PcReceiveAction PcReceiveActionAt(HWND window, POINT point) {
    RECT client{};
    GetClientRect(window, &client);
    const float x = client.right * 0.5f, y = client.bottom * 0.5f;
    const float radius = std::min(client.right, client.bottom) * kPcReceiveRadiusRatio;
    const auto inside = [&](PcReceiveAction action) {
        const auto r = PcReceiveControl(action, x, y, radius);
        return point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom;
    };
    if (g_pcOffer->complete) return inside(PcReceiveAction::OpenFolder) ? PcReceiveAction::OpenFolder : PcReceiveAction::None;
    if (g_pcOffer->accepted || g_pcOffer->failed) return PcReceiveAction::None;
    if (g_pcReceiveCloseVisible && inside(PcReceiveAction::Close)) return PcReceiveAction::Close;
    return inside(PcReceiveAction::Confirm) ? PcReceiveAction::Confirm : PcReceiveAction::None;
}

bool PcReceiveDragExceeded(HWND window, POINT point) {
    const UINT dpi = GetDpiForWindow(window);
    return std::abs(point.x - g_pcReceivePressPoint.x) > std::max(MulDiv(5, dpi, 96), GetSystemMetricsForDpi(SM_CXDRAG, dpi)) ||
        std::abs(point.y - g_pcReceivePressPoint.y) > std::max(MulDiv(5, dpi, 96), GetSystemMetricsForDpi(SM_CYDRAG, dpi));
}

RECT PcReceivePlacement(const RECT& work, UINT dpi) {
    const int margin = MulDiv(18, dpi, 96);
    const int size = std::max<LONG>(1, std::min({static_cast<LONG>(MulDiv(kPcReceiveWindowDip, dpi, 96)),
        work.right - work.left - 2 * margin, work.bottom - work.top - 2 * margin}));
    return {work.right - margin - size, work.bottom - margin - size, work.right - margin, work.bottom - margin};
}

void SetPcReceiveRegion(HWND window, int diameter) {
    const int edge = std::max(1, diameter * 3 / 100);
    const HRGN region = CreateEllipticRgn(edge, edge, diameter - edge, diameter - edge);
    if (region && !SetWindowRgn(window, region, FALSE)) DeleteObject(region);
}

void ConfirmPcBall() {
    auto& offer = *g_pcOffer;
    if (offer.complete) {
        ShellExecuteW(g_wnd, L"open", offer.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g_quit = true;
    } else if (!offer.accepted && !offer.failed) {
        if (!docky::pairing::pc_receive::Answer("accept\n", 7)) g_quit = true;
        else offer.accepted = true;
    }
}

void ClosePcBall() {
    if (IsPcReceiveBall() && !g_pcOffer->accepted && !g_pcOffer->failed)
        docky::pairing::pc_receive::Answer("decline\n", 8);
    g_quit = true;
}

float PhoneBallCenterY(float height, float dpi) { return (height - 110.0f * dpi) * 0.5f; }

SIZE PhoneWindowSize(const RECT& work, float scale) {
    const int margin = static_cast<int>(20 * scale);
    return {std::min(static_cast<LONG>(g_opt.sizeW * scale), work.right - work.left - margin),
            std::min(static_cast<LONG>(g_opt.sizeH * scale), work.bottom - work.top - margin)};
}

void FitPhoneWindow(HWND window, const RECT& proposed, unsigned dpi) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto size = PhoneWindowSize(monitor.rcWork, dpi / 96.0f);
    const int x = std::clamp(proposed.left, monitor.rcWork.left, monitor.rcWork.right - size.cx);
    const int y = std::clamp(proposed.top, monitor.rcWork.top, monitor.rcWork.bottom - size.cy);
    SetWindowPos(window, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateRadiusLabel() {
    const int percent = static_cast<int>(std::lround(g_opt.radius * 100.0f / liquid::settings::kPhoneBallDefault));
    const auto label = Ui(L"扫描球大小  ") + std::to_wstring(percent) + L"%";
    SetWindowTextW(g_sizeLabel, label.c_str());
}

void SaveRadius() {
    KillTimer(g_wnd, kSaveRadiusTimer);
    if (!g_radiusDirty) return;
    g_radiusDirty = liquid::settings::WritePhoneBallRadius(g_opt.settingsPath, g_preferredRadius)
        != liquid::settings::SaveStatus::Saved;
}

void ChangeRadius(int value) {
    const int next = std::clamp(value, g_minRadius, g_maxRadius);
    if (next == static_cast<int>(g_opt.radius)) return;
    g_opt.radius = static_cast<float>(next);
    g_preferredRadius = next;
    g_radiusDirty = true;
    UpdateRadiusLabel();
    SetTimer(g_wnd, kSaveRadiusTimer, 260, nullptr);
}

float SliderPress(double now) {
    if (g_reduceMotion) return g_pressTo;
    const float left = 1.0f - std::clamp(static_cast<float>((now - g_pressAt) / 0.15), 0.0f, 1.0f);
    return std::lerp(g_pressTo, g_pressFrom, left * left * left);
}

void SetSliderPress(bool pressed) {
    if (g_sliderHeld == pressed) return;
    const double now = Now();
    g_pressFrom = SliderPress(now);
    g_pressTo = pressed ? 1.0f : 0.0f;
    g_pressAt = now;
    g_sliderHeld = pressed;
    InvalidateRect(g_slider, nullptr, FALSE);
}

float SliderInset(HWND slider) { return 31.0f * GetDpiForWindow(slider) / 96.0f; }

LRESULT CALLBACK SliderProc(HWND slider, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    const auto select = [slider](int x) {
        RECT r{};
        GetClientRect(slider, &r);
        const float inset = SliderInset(slider);
        const float value = std::clamp((x - inset) / std::max(1.0f, r.right - 2.0f * inset), 0.0f, 1.0f);
        ChangeRadius(static_cast<int>(std::lround(std::lerp(static_cast<float>(g_minRadius), static_cast<float>(g_maxRadius), value))));
        SendMessageW(slider, TBM_SETPOS, FALSE, static_cast<LPARAM>(g_opt.radius));
        InvalidateRect(slider, nullptr, FALSE);
    };
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        DRAWITEMSTRUCT draw{};
        draw.hwndItem = slider;
        draw.CtlID = kBallSlider;
        draw.hDC = BeginPaint(slider, &paint);
        GetClientRect(slider, &draw.rcItem);
        PaintPhoneItem(draw);
        EndPaint(slider, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(slider);
        SetCapture(slider);
        SetSliderPress(true);
        select(GET_X_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (g_sliderHeld) { select(GET_X_LPARAM(lp)); return 0; }
        break;
    case WM_LBUTTONUP:
        if (g_sliderHeld) {
            select(GET_X_LPARAM(lp));
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        SetSliderPress(false);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(slider, SliderProc, 1);
        break;
    }
    return DefSubclassProc(slider, message, wp, lp);
}

void UpdateQr() {
    const bool available = g_qrSize > 0 && g_opt.codeMode == 4;
    EnableWindow(g_qrButton, available);
    if (!available) g_showQr = false;
    const int showSize = g_showQr || g_connectedAt > 0 ? SW_HIDE : SW_SHOW;
    ShowWindow(g_sizeLabel, showSize);
    ShowWindow(g_slider, showSize);
    SetWindowTextW(g_qrButton, g_showQr ? Ui(L"显示玻璃球") : Ui(L"改用二维码"));
}

void SetReceiveStatus(const wchar_t* status) {
    SetWindowTextW(g_status, status);
    ShowWindow(g_status, SW_SHOW);
}

void CopyReceivedText() {
    if (g_lastText.empty()) return;
    if (!OpenClipboard(g_wnd)) { SetReceiveStatus(Ui(L"暂时无法打开剪贴板")); return; }
    const size_t size = (g_lastText.size() + 1) * sizeof(wchar_t);
    const HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, size);
    bool copied = false;
    if (storage) {
        if (void* buffer = GlobalLock(storage)) {
            memcpy(buffer, g_lastText.c_str(), size);
            GlobalUnlock(storage);
            copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, storage) != nullptr;
        }
        if (!copied) GlobalFree(storage);
    }
    CloseClipboard();
    SetReceiveStatus(copied ? Ui(L"文字已复制") : Ui(L"无法复制文字"));
}

bool ReceiveTransfer(std::wstring_view message) {
    if (message.starts_with(L"receive-begin:")) {
        if (!g_incoming.Begin(g_receiveFolder, message.substr(14))) return false;
        g_transferring = true;
        SetReceiveStatus(Ui(L"正在接收文件…"));
    } else if (message.starts_with(L"receive-data:")) {
        return g_incoming.Append(message.substr(13));
    } else if (message == L"receive-finish") {
        if (!g_incoming.Commit()) return false;
        ++g_filesReceived;
        RevealAction(2);
        LayoutPhoneControls(g_wnd);
    } else if (message.starts_with(L"receive-text:")) {
        const auto text = message.substr(13);
        if (text.empty() || text.size() > 65536 || text.find(L'\0') != std::wstring_view::npos) return false;
        g_lastText.clear();
        g_lastText.reserve(text.size() + 1);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) g_lastText += L'\r';
            g_lastText += text[i];
        }
        SetWindowTextW(g_receivedText, g_lastText.c_str());
        RevealAction(0);
        RevealAction(1);
        LayoutPhoneControls(g_wnd);
    } else if (message == L"receive-done") {
        g_transferring = false;
        g_finishedAt = Now();
        g_transferProgress = 1.0f;
        const auto status = g_filesReceived > 0
            ? Ui(L"已接收 ") + std::to_wstring(g_filesReceived) + Ui(L" 个文件") + (g_lastText.empty() ? L"" : Ui(L"和文字"))
            : std::wstring(g_lastText.empty() ? Ui(L"接收完成") : Ui(L"已接收文字"));
        SetReceiveStatus(status.c_str());
    } else if (message == L"receive-abort") {
        g_transferring = false;
        g_incoming.Cancel();
        g_transferProgress = 0.0f;
        SetReceiveStatus(Ui(L"传输已中断"));
    } else {
        return false;
    }
    return true;
}

void ReplyPhone(const std::wstring& reply) {
    docky::pairing::ReplyToPhoneHost(g_phoneHost, reply.c_str());
}

bool HandlePhoneTrust(std::wstring_view message) {
    using namespace docky::pairing;
    namespace json = dock::json;
    if (message == L"trust-list") {
        auto entries = json::Value{};
        entries.type = json::Value::Type::Array;
        for (const auto& phone : ReadTrustedPhones()) {
            if (!g_targetPhone.empty() && phone.id != g_targetPhone) continue;
            auto entry = json::Value::MakeObject();
            entry.Set("id", json::Value::MakeString(json::ToUtf8(phone.id)));
            entry.Set("secret", json::Value::MakeString(json::ToUtf8(phone.secret)));
            entry.Set("name", json::Value::MakeString(json::ToUtf8(phone.name)));
            entries.array.push_back(std::move(entry));
        }
        wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD chars = ARRAYSIZE(hostname);
        GetComputerNameW(hostname, &chars);
        auto result = json::Value::MakeObject();
        result.Set("computer", json::Value::MakeString(json::ToUtf8(hostname)));
        result.Set("phones", std::move(entries));
        ReplyPhone(L"ok:" + json::FromUtf8(json::Write(result)));
    } else if (message.starts_with(L"trust-check:")) {
        const auto id = message.substr(12);
        const auto phones = ReadTrustedPhones();
        const auto phone = std::find_if(phones.begin(), phones.end(), [id](const auto& item) { return item.id == id; });
        ReplyPhone(phone == phones.end() ? L"error" : L"ok");
    } else if (message.starts_with(L"trust-add:") && g_connectedAt > 0) {
        const auto fields = message.substr(10);
        if (fields.size() <= 98 || fields[32] != L':' || fields[97] != L':') {
            ReplyPhone(L"error");
            return true;
        }
        const TrustedPhone phone{std::wstring(fields.substr(0, 32)), std::wstring(fields.substr(33, 64)),
            std::wstring(fields.substr(98))};
        const bool saved = g_activePhone.empty() && SaveTrustedPhone(phone);
        if (saved) g_activePhone = phone.id;
        ReplyPhone(saved ? L"ok" : L"error");
    } else return false;
    return true;
}

void ShowPhoneSettings(HWND owner) {
    using namespace docky::pairing;
    for (;;) {
        auto phones = ReadTrustedPhones();
        const int choice = ShowStyledPhoneSettings(owner, g_receiveFolder, phones, g_opt.settingsPath);
        if (choice == phone_settings::kChangeFolder) {
            const auto folder = ChooseReceiveFolder(owner, g_receiveFolder);
            if (!folder.empty()) {
                if (liquid::settings::WritePhoneReceiveFolder(g_opt.settingsPath, folder) == liquid::settings::SaveStatus::Saved)
                    g_receiveFolder = folder;
                else MessageBoxW(owner, Ui(L"无法保存接收文件夹设置。"), L"DockDrop", MB_OK | MB_ICONWARNING);
            }
        } else if (choice == phone_settings::kOpenFolder) {
            ShellExecuteW(owner, L"open", g_receiveFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (choice >= phone_settings::kFirstPhone &&
                   static_cast<size_t>(choice - phone_settings::kFirstPhone) < phones.size()) {
            const auto& phone = phones[choice - phone_settings::kFirstPhone];
            const std::wstring question = std::wstring(Ui(L"移除此手机？下次连接需要重新扫码。"))
                + L"\n\n" + phone.name;
            if (MessageBoxW(owner, question.c_str(), L"DockDrop", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
                continue;
            if (!ForgetTrustedPhone(phone.id)) {
                MessageBoxW(owner, Ui(L"无法移除手机。"), L"DockDrop", MB_OK | MB_ICONWARNING);
            } else {
                if (g_targetPhone == phone.id) g_targetPhone.clear();
                if (g_activePhone == phone.id) ResetPairing();
                else if (g_connectedAt == 0) ReplyPhone(L"trust-refresh");
            }
        } else return;
    }
}

void ReceivePhoneMessage(std::wstring_view message) {
    if (g_quit || HandlePhoneTrust(message)) return;
    if (g_connectedAt > 0) {
        if ((message == L"receive-offer:" || message == L"send-offer:") && !g_activePhone.empty()) {
            if (!g_receivePrompt.empty() || !PostMessageW(g_wnd, kPhoneOffer, 0, 0)) {
                ReplyPhone(L"error");
            } else {
                g_receivePrompt = g_activePhone;
                g_offerSending = message == L"send-offer:";
            }
        } else if (message.starts_with(L"transfer-progress:") && g_finishedAt < 0) {
            const std::wstring digits(message.substr(18));
            if (digits.empty() || digits.size() > 5 || digits.find_first_not_of(L"0123456789") != std::wstring::npos)
                return;
            const float fraction = std::clamp(std::wcstol(digits.c_str(), nullptr, 10) / 10000.0f, 0.0f, 0.995f);
            g_transferProgress = std::max(g_transferProgress, fraction);
            g_transferring = true;
        } else if (message.starts_with(L"receive-")) {
            const bool accepted = ReceiveTransfer(message);
            if (!accepted) {
                g_incoming.Cancel();
                g_transferring = false;
                g_transferProgress = 0.0f;
                SetReceiveStatus(Ui(L"无法接收内容"));
            }
            docky::pairing::ReplyToPhoneHost(g_phoneHost, accepted ? L"ok" : L"error");
        } else if (message == L"out-meta" || message.starts_with(L"out-read:")) {
            const auto data = message == L"out-meta" ? g_outgoing.Manifest() : g_outgoing.Read(message.substr(9));
            const auto reply = data.empty() ? std::wstring(L"error") : L"ok:" + data;
            docky::pairing::ReplyToPhoneHost(g_phoneHost, reply.c_str());
        } else if (message == L"out-start") {
            g_transferring = true;
            SetReceiveStatus(Ui(L"正在发送到手机…"));
        } else if (message == L"out-done" || message == L"out-failed") {
            g_transferring = false;
            if (message == L"out-done") {
                g_finishedAt = Now();
                g_transferProgress = 1.0f;
                docky::pairing::WidgetQueue().Acknowledge(g_widgetDraft);
            } else {
                g_transferProgress = 0.0f;
            }
            g_widgetDraft = {};
            g_outgoing.Clear();
            SetReceiveStatus(message == L"out-done" ? Ui(L"已发送到手机") : Ui(L"发送未完成"));
        } else if (message == L"error") {
            g_transferring = false;
            g_incoming.Cancel();
            if (g_finishedAt < 0) g_transferProgress = 0.0f;
            SetReceiveStatus(Ui(L"连接已关闭"));
        }
        // Nothing left to interrupt: the question no longer applies.
        if (g_confirming && !g_transferring) ShowBackConfirm(false);
        return;
    }
    if (message.starts_with(L"qr:")) {
        const auto payload = message.substr(3);
        const auto divider = payload.find(L':');
        if (divider == std::wstring_view::npos || divider != 2 || payload[0] < L'0' || payload[0] > L'9'
            || payload[1] < L'0' || payload[1] > L'9') return;
        const int size = (payload[0] - L'0') * 10 + payload[1] - L'0';
        const auto cells = payload.substr(divider + 1);
        if (size < 21 || size > 77 || cells.size() != static_cast<size_t>(size * size)
            || cells.find_first_not_of(L"01") != std::wstring_view::npos) return;
        ZeroMemory(g_qr, sizeof(g_qr));
        for (size_t k = 0; k < cells.size(); ++k) {
            if (cells[k] == L'1') g_qr[k >> 5] |= 1u << (k & 31);
        }
        g_qrSize = size;
    } else if (message.starts_with(L"cells:")) {
        const auto cells = message.substr(6);
        if (cells.size() != 168 || cells.find_first_not_of(L"01") != std::wstring_view::npos) return;
        ZeroMemory(g_opt.codeBits, sizeof(g_opt.codeBits));
        for (size_t k = 0; k < cells.size(); ++k) {
            if (cells[k] == L'1') g_opt.codeBits[k >> 5] |= 1u << (k & 31);
        }
        g_opt.codeMode = 4;
        UpdateQr();
    } else if (message.starts_with(L"status:")) {
        SetWindowTextW(g_status, Ui(std::wstring(message.substr(7)).c_str()));
    } else if ((message == L"paired" && g_opt.codeMode == 4) ||
               (message.starts_with(L"paired:") && docky::pairing::TrustHex(message.substr(7), 32))) {
        if (message.starts_with(L"paired:")) g_activePhone.assign(message.substr(7));
        g_opt.codeMode = 4;
        g_connectedAt = Now();
        ZeroMemory(g_qr, sizeof(g_qr));
        g_qrSize = 0;
        UpdateQr();
        ShowWindow(g_guide, SW_HIDE);
        ShowWindow(g_qrButton, SW_HIDE);
        LayoutPhoneControls(g_wnd);
        ShowWindow(g_sendFiles, SW_HIDE);
        ShowWindow(g_sendText, SW_HIDE);
        ShowWindow(g_status, SW_HIDE);
        ShowWindow(g_back, SW_SHOW);
    } else if (message == L"clear" || message == L"error" || message == L"runtime") {
        g_opt.codeMode = 0;
        ZeroMemory(g_opt.codeBits, sizeof(g_opt.codeBits));
        ZeroMemory(g_qr, sizeof(g_qr));
        g_qrSize = 0;
        UpdateQr();
        if (message == L"error") SetWindowTextW(g_status, Ui(L"无法启动配对。请关闭后重试。"));
        if (message == L"runtime") SetWindowTextW(g_status, Ui(L"配对需要 Microsoft Edge WebView2 Runtime。"));
    }
}

void LayoutPhoneControls(HWND window) {
    const int dpi = static_cast<int>(GetDpiForWindow(window));
    const auto dip = [dpi](int n) { return MulDiv(n, dpi, 96); };
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    SetWindowPos(g_surface, HWND_BOTTOM, 0, 0, width, height, SWP_NOACTIVATE);
    if (dpi != g_uiDpi) {
        const HFONT next = CreateFontW(-dip(14), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        if (next) {
            for (HWND child : {g_status, g_guide, g_qrButton, g_close, g_back, g_sizeLabel, g_receiveSettings,
                 g_receivedText, g_copyText, g_openFolder, g_sendFiles, g_sendText,
                 g_confirmText, g_confirmStay, g_confirmLeave}) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
            }
            if (g_uiFont) DeleteObject(g_uiFont);
            g_uiFont = next;
            g_uiDpi = dpi;
        }
    }
    const int statusWidth = std::max(dip(100), std::min(dip(270), width - dip(230)));
    const int inset = dip(20), button = dip(36);
    MoveWindow(g_status, (width - statusWidth) / 2, inset, statusWidth, button, TRUE);
    const float scale = dpi / 96.0f;
    const float ballY = PhoneBallCenterY(static_cast<float>(height), scale);
    const float clearRadius = std::min(width * 0.5f - dip(24), ballY - dip(76)) / scale - 10.0f;
    g_maxRadius = std::clamp(static_cast<int>(clearRadius), liquid::settings::kPhoneBallMinimum,
        liquid::settings::kPhoneBallMaximum);
    g_minRadius = std::max(liquid::settings::kPhoneBallMinimum, std::min(130, g_maxRadius - 30));
    g_opt.radius = static_cast<float>(std::clamp(g_preferredRadius, g_minRadius, g_maxRadius));
    SendMessageW(g_slider, TBM_SETRANGEMIN, FALSE, g_minRadius);
    SendMessageW(g_slider, TBM_SETRANGEMAX, FALSE, g_maxRadius);
    SendMessageW(g_slider, TBM_SETPOS, FALSE, static_cast<LPARAM>(g_opt.radius));
    UpdateRadiusLabel();
    const int sliderWidth = std::min(dip(290), width - dip(68));
    MoveWindow(g_sizeLabel, (width - sliderWidth) / 2, height - dip(175), sliderWidth, dip(22), TRUE);
    MoveWindow(g_slider, (width - sliderWidth) / 2, height - dip(152), sliderWidth, dip(44), TRUE);
    const int guideWidth = std::min(dip(424), width - dip(24));
    MoveWindow(g_guide, (width - guideWidth) / 2, height - dip(102), guideWidth, dip(26), TRUE);
    MoveWindow(g_qrButton, width / 2 - dip(66), height - inset - button, dip(132), button, TRUE);
    MoveWindow(g_sendFiles, inset, height - inset - button, dip(112), button, TRUE);
    MoveWindow(g_sendText, width - inset - dip(112), height - inset - button, dip(112), button, TRUE);
    MoveWindow(g_close, width - inset - button, inset, button, button, TRUE);
    MoveWindow(g_back, inset, inset, button, button, TRUE);
    MoveWindow(g_receiveSettings, inset + (g_connectedAt > 0 ? button + dip(7) : 0), inset, dip(95), button, TRUE);
    const int receiveWidth = std::min(dip(414), width - dip(36));
    const int textHeight = dip(86);
    MoveWindow(g_receivedText, (width - receiveWidth) / 2, height - dip(166), receiveWidth, textHeight, TRUE);
    RECT textArea{dip(37), dip(10), receiveWidth - dip(37), textHeight - dip(9)};
    SendMessageW(g_receivedText, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&textArea));
    const bool bothActions = IsWindowVisible(g_copyText) && IsWindowVisible(g_openFolder);
    const int actionWidth = dip(114), gap = dip(14);
    const int centered = (width - actionWidth) / 2;
    const int pairLeft = (width - (2 * actionWidth + gap)) / 2;
    MoveWindow(g_copyText, bothActions ? pairLeft : centered, height - inset - button, actionWidth, button, TRUE);
    MoveWindow(g_openFolder, bothActions ? pairLeft + actionWidth + gap : centered, height - inset - button, actionWidth, button, TRUE);
    MoveWindow(g_confirmText, (width - receiveWidth) / 2, height - dip(166), receiveWidth, textHeight, TRUE);
    MoveWindow(g_confirmStay, pairLeft, height - inset - button, actionWidth, button, TRUE);
    MoveWindow(g_confirmLeave, pairLeft + actionWidth + gap, height - inset - button, actionWidth, button, TRUE);
}

LRESULT CALLBACK ReceivedTextProc(HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        DRAWITEMSTRUCT item{};
        item.CtlID = kReceivedText;
        item.hwndItem = edit;
        item.hDC = BeginPaint(edit, &paint);
        item.itemState = GetFocus() == edit ? ODS_FOCUS : 0;
        GetClientRect(edit, &item.rcItem);
        PaintPhoneItem(item);
        EndPaint(edit, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        static int partial = 0;
        partial += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = partial / WHEEL_DELTA;
        partial %= WHEEL_DELTA;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (steps && lines == WHEEL_PAGESCROLL) {
            SendMessageW(edit, WM_VSCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
        } else if (steps && lines) {
            SendMessageW(edit, EM_LINESCROLL, 0, -steps * static_cast<int>(lines));
        }
        InvalidateRect(edit, nullptr, FALSE);
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(edit, ReceivedTextProc, 1);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(edit, nullptr, FALSE);
        break;
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_MOUSEMOVE: case WM_TIMER:
    case WM_VSCROLL: case WM_SETTEXT: case EM_SETSEL: case EM_LINESCROLL: case EM_SCROLL: case EM_SCROLLCARET: {
        // The Edit paints selection, caret moves and scrolling straight to the screen (ScrollWindow and its
        // own WM_CTLCOLOR fill), outside WM_PAINT, which leaves flat patches and smeared glass edges. Repaint the
        // whole field through the glass path right after.
        if (message == WM_MOUSEMOVE && !(wp & MK_LBUTTON)) break;
        const LRESULT result = DefSubclassProc(edit, message, wp, lp);
        RedrawWindow(edit, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        return result;
    }
    }
    return DefSubclassProc(edit, message, wp, lp);
}

void CreatePhoneControls(HWND window) {
    const auto create = [window](const wchar_t* cls, const wchar_t* text, DWORD style, UINT id = 0) {
        HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return child;
    };
    g_status = create(L"STATIC", Ui(L"正在准备手机配对…"), WS_VISIBLE | SS_OWNERDRAW, kStatus);
    g_guide = create(L"STATIC", kScanLink, WS_VISIBLE | SS_OWNERDRAW, kGuide);
    g_qrButton = create(L"BUTTON", Ui(L"改用二维码"), WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW, kQrToggle);
    g_close = create(L"BUTTON", Ui(L"关闭"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kClose);
    g_back = create(L"BUTTON", Ui(L"返回"), WS_TABSTOP | BS_OWNERDRAW, kBack);
    g_receiveSettings = create(L"BUTTON", Ui(L"接收设置"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kReceiveSettings);
    g_copyText = create(L"BUTTON", Ui(L"复制文字"), WS_TABSTOP | BS_OWNERDRAW, kCopyText);
    g_openFolder = create(L"BUTTON", Ui(L"打开文件夹"), WS_TABSTOP | BS_OWNERDRAW, kOpenFolder);
    g_sendFiles = create(L"BUTTON", Ui(L"选择发送文件"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kSendFiles);
    g_sendText = create(L"BUTTON", Ui(L"加入剪贴板文字"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, kSendText);
    g_confirmText = create(L"STATIC", Ui(L"返回会中断当前传输"), SS_OWNERDRAW, kConfirmText);
    g_confirmStay = create(L"BUTTON", Ui(L"继续传输"), WS_TABSTOP | BS_OWNERDRAW, kConfirmStay);
    g_confirmLeave = create(L"BUTTON", Ui(L"中断并返回"), WS_TABSTOP | BS_OWNERDRAW, kConfirmLeave);
    g_receivedText = create(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, kReceivedText);
    SetWindowSubclass(g_receivedText, ReceivedTextProc, 1, 0);
    g_sizeLabel = create(L"STATIC", Ui(L"扫描球大小"), WS_VISIBLE | SS_OWNERDRAW, kBallSize);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    g_slider = create(TRACKBAR_CLASSW, Ui(L"扫描球大小"), WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS, kBallSlider);
    SetWindowSubclass(g_slider, SliderProc, 1, 0);
    SendMessageW(g_slider, TBM_SETLINESIZE, 0, 4);
    SendMessageW(g_slider, TBM_SETPAGESIZE, 0, 16);
    BOOL animation = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    g_reduceMotion = !animation;
    LayoutPhoneControls(window);
}


// Match studioDark in bubble.hlsl so each HWND can draw an opaque, antialiased
// tile over the DComp surface. The glass bevel refracts this same backdrop.
void PaintControlBackdrop(const DRAWITEMSTRUCT& item, uint32_t* pixels, int width, int height) {
    POINT origin{};
    MapWindowPoints(item.hwndItem, g_wnd, &origin, 1);
    RECT client{};
    GetClientRect(g_wnd, &client);
    if (client.right < 1 || client.bottom < 1) return;
    const auto smooth = [](float a, float b, float x) {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const auto backdrop = [&](float x, float y) {
        const float u = (origin.x + x) / client.right - 0.5f;
        const float v = (origin.y + y) / client.bottom;
        const float light = std::lerp(std::lerp(0.46f, 0.30f, smooth(0.0f, 0.6f, v)),
            0.10f, smooth(0.45f, 0.95f, v));
        return light * (1.0f - 0.25f * u * u);
    };
    const auto byte = [](float channel) { return static_cast<uint32_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255)); };
    const bool glass = item.CtlID != kBallSize && item.CtlID != kBallSlider;
    const bool field = item.CtlID == kReceivedText || item.CtlID == kConfirmText, circle = item.CtlID == kClose || item.CtlID == kBack;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const float scale = GetDpiForWindow(item.hwndItem) / 96.0f;
    const float halfW = (width - 1.4f) * 0.5f, halfH = (height - 1.4f) * 0.5f;
    const float radius = std::min(halfW, halfH);
    const auto material = glass ? dock::ControlLensGlass(radius * 2) : dock::GlassParameters{};
    const float bend = std::clamp(material.refraction * (material.refractiveIndex - 1), 0.0f, 1.5f);
    const float bevel = std::clamp(material.bevelWidth * scale, 2.0f * scale, 10.0f * scale);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float base = backdrop(px, py);
            float r = base * 0.95f, g = base * 0.97f, b = base;
            if (glass && radius > 0) {
                const float dx = px - width * 0.5f, dy = py - height * 0.5f;
                const float qx = std::abs(dx) - (halfW - radius), qy = std::abs(dy) - (halfH - radius);
                float distance = std::max(qx, qy) - radius;
                float nx = qx > qy ? 1.0f : 0.0f, ny = 1.0f - nx;
                if (circle || qx > 0 && qy > 0) {
                    const float ax = circle ? std::abs(dx) : qx, ay = circle ? std::abs(dy) : qy;
                    // A fourth-order corner meets its straight edge at zero curvature.
                    const float ux = ax / radius, uy = ay / radius;
                    const float length = circle ? std::hypot(ax, ay)
                        : radius * std::sqrt(std::sqrt(ux*ux*ux*ux + uy*uy*uy*uy));
                    distance = length - radius;
                    const float gx = circle ? ux : ux*ux*ux, gy = circle ? uy : uy*uy*uy;
                    const float gradient = std::max(0.001f, std::hypot(gx, gy));
                    nx = gx / gradient;
                    ny = gy / gradient;
                    if (!circle && length > radius * 0.5f) distance *= std::pow(std::max(length / radius, 0.001f), 3.0f) / gradient;
                }
                const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
                if (coverage > 0) {
                    nx = std::copysign(nx, dx);
                    ny = std::copysign(ny, dy);
                    const float grazing = 1.0f - smooth(0, bevel, -distance);
                    const float offset = grazing * grazing * bevel * bend * (pressed ? 3.1f : 2.1f);
                    const auto lens = [&](float dispersion) {
                        return backdrop(px - nx * offset * dispersion, py - ny * offset * dispersion);
                    };
                    const float key = std::clamp(-0.44f * nx - 0.90f * ny, 0.0f, 1.0f);
                    const float rim = 1.0f - smooth(0.1f, 1.8f * scale, -distance);
                    const float bounce = std::clamp(0.42f * nx + 0.75f * ny, 0.0f, 1.0f);
                    const float reflection = grazing * (0.023f + key * 0.086f)
                        + rim * (0.037f + key * 0.180f + bounce * 0.040f + (focused ? 0.25f : 0.0f));
                    const float tint = (field ? 0.031f : 0.040f) + (pressed ? 0.055f : 0.0f);
                    const float transmission = field ? 0.54f : 0.73f;
                    r = std::lerp(r, lens(0.92f) * transmission * 0.95f + tint + reflection, coverage);
                    g = std::lerp(g, lens(1.0f) * transmission * 0.97f + tint * 1.25f + reflection, coverage);
                    b = std::lerp(b, lens(1.10f) * transmission + tint * 1.62f + reflection, coverage);
                }
            }
            pixels[static_cast<size_t>(y) * width + x] = 0xff000000u | (byte(r) << 16) | (byte(g) << 8) | byte(b);
        }
    }
}

void PaintPhoneControl(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    if (item.CtlID == kClose) {
        using namespace Gdiplus;
        const float width = static_cast<float>(rect.right - rect.left);
        const float height = static_cast<float>(rect.bottom - rect.top);
        Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
        Pen cross(Color(237, 244, 255), std::max(height / 18.0f, 1.0f));
        cross.SetStartCap(LineCapRound);
        cross.SetEndCap(LineCapRound);
        const float cx = width * 0.5f, cy = height * 0.5f, half = height * 0.135f;
        graphics.DrawLine(&cross, cx - half, cy - half, cx + half, cy + half);
        graphics.DrawLine(&cross, cx - half, cy + half, cx + half, cy - half);
    } else if (item.CtlID == kConfirmText) {
        const HGDIOBJ oldFont = g_uiFont ? SelectObject(item.hDC, g_uiFont) : nullptr;
        SetBkMode(item.hDC, TRANSPARENT);
        RECT title = rect, detail = rect;
        title.bottom = (rect.top + rect.bottom) / 2 + 2;
        detail.top = title.bottom + 2;
        SetTextColor(item.hDC, kControlText);
        wchar_t heading[96]{};
        GetWindowTextW(item.hwndItem, heading, ARRAYSIZE(heading));
        DrawTextW(item.hDC, heading, -1, &title, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        SetTextColor(item.hDC, kMutedText);
        DrawTextW(item.hDC, Ui(L"未完成的文件不会保留"), -1, &detail, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (oldFont) SelectObject(item.hDC, oldFont);
    } else if (item.CtlID == kBack) {
        using namespace Gdiplus;
        const float height = static_cast<float>(rect.bottom - rect.top);
        Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
        Pen chevron(Color(237, 244, 255), std::max(height / 18.0f, 1.0f));
        chevron.SetStartCap(LineCapRound);
        chevron.SetEndCap(LineCapRound);
        chevron.SetLineJoin(LineJoinRound);
        // Same stroke and extent as the close cross, nudged right so the chevron sits optically centred.
        const float cx = (rect.right - rect.left) * 0.5f - height * 0.03f, cy = height * 0.5f, half = height * 0.15f;
        const PointF points[] = {{cx + half * 0.55f, cy - half}, {cx - half * 0.45f, cy}, {cx + half * 0.55f, cy + half}};
        graphics.DrawLines(&chevron, points, 3);
    } else {
        wchar_t text[64]{};
        GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
        const HGDIOBJ oldFont = g_uiFont ? SelectObject(item.hDC, g_uiFont) : nullptr;
        SetTextColor(item.hDC, disabled || item.CtlID == kGuide || item.CtlID == kBallSize ? kMutedText : kControlText);
        SetBkMode(item.hDC, TRANSPARENT);
        const UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        DrawTextW(item.hDC, text, -1, &rect, g_opt.pcPreview ? flags | DT_END_ELLIPSIS : flags);
        if (oldFont) SelectObject(item.hDC, oldFont);
    }
}

// Refract the track and the studio backdrop beneath the shared Lab thumb.
// This local pass operates on the slider's own small backing bitmap.
void PaintSliderLens(uint32_t* pixels, int width, int height, const dock::LabSliderShape& shape) {
    const auto& thumb = shape.thumb;
    const float cx = (thumb.left + thumb.right) * 0.5f, cy = (thumb.top + thumb.bottom) * 0.5f;
    const float halfHeight = (thumb.bottom - thumb.top) * 0.5f;
    const float axis = (thumb.right - thumb.left) * 0.5f - halfHeight;
    const auto material = dock::ControlLensGlass(halfHeight * 2.0f);
    const float strength = std::clamp(material.refraction * (material.refractiveIndex - 1.0f), 0.0f, 1.2f);
    const auto size = static_cast<size_t>(width) * height;
    const std::vector<uint32_t> source(pixels, pixels + size);
    const auto sample = [&](float x, float y, unsigned shift) {
        x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        const int right = std::min(ix + 1, width - 1), bottom = std::min(iy + 1, height - 1);
        const auto channel = [&](int sx, int sy) {
            return static_cast<float>((source[static_cast<size_t>(sy) * width + sx] >> shift) & 255u);
        };
        return std::lerp(std::lerp(channel(ix, iy), channel(right, iy), x - ix),
            std::lerp(channel(ix, bottom), channel(right, bottom), x - ix), y - iy);
    };
    for (int y = std::max(0, static_cast<int>(thumb.top) - 1); y < std::min(height, static_cast<int>(std::ceil(thumb.bottom)) + 1); ++y) {
        for (int x = std::max(0, static_cast<int>(thumb.left) - 1); x < std::min(width, static_cast<int>(std::ceil(thumb.right)) + 1); ++x) {
            const float sx = x + 0.5f - cx, sy = y + 0.5f - cy;
            const float dx = sx - std::clamp(sx, -axis, axis);
            const float d = std::hypot(dx, sy);
            const float coverage = std::clamp(halfHeight + 0.5f - d, 0.0f, 1.0f);
            if (coverage <= 0) continue;
            const float depth = std::clamp(1.0f - d / halfHeight, 0.0f, 1.0f);
            const float bend = strength * (1.0f - depth) * (1.0f - depth);
            const float shine = (1.0f - std::min(depth * 7.0f, 1.0f))
                * (0.22f + 0.62f * std::clamp(-sy / halfHeight, 0.0f, 1.0f));
            const auto index = static_cast<size_t>(y) * width + x;
            uint32_t result = 0xff000000u;
            for (unsigned shift : {0u, 8u, 16u}) {
                float light = sample(x - dx * bend, y - sy * bend, shift);
                const float reflection = (1.0f - shape.press) * 0.63f + shine * 0.45f;
                light = std::lerp(light, 255.0f, std::min(reflection, 1.0f));
                const float original = static_cast<float>((source[index] >> shift) & 255u);
                result |= static_cast<uint32_t>(std::lround(std::lerp(original, light, coverage))) << shift;
            }
            pixels[index] = result;
        }
    }
}

void PaintBallSlider(const DRAWITEMSTRUCT& item, uint32_t* pixels, int width, int height) {
    if (!g_sliderTarget) {
        if (!g_sliderFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_sliderFactory.GetAddressOf()))) return;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(g_sliderFactory->CreateDCRenderTarget(&props, &g_sliderTarget))) return;
        if (FAILED(g_sliderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_sliderBrush))) {
            g_sliderTarget.Reset();
            return;
        }
    }
    if (FAILED(g_sliderTarget->BindDC(item.hDC, &item.rcItem))) return;
    const float dpi = GetDpiForWindow(g_slider) / 96.0f;
    const float inset = SliderInset(g_slider);
    const float value = (g_opt.radius - g_minRadius) / std::max(g_maxRadius - g_minRadius, 1);
    const float press = SliderPress(Now());
    const auto shape = dock::LabSlider(inset, width - inset, height * 0.5f, value, dpi * 0.91f, press);
    g_sliderTarget->BeginDraw();
    auto trackOnly = shape;
    trackOnly.press = 1.0f;
    dock::DrawLabSlider(g_sliderTarget.Get(), g_sliderBrush.Get(), trackOnly, true, true);
    if (FAILED(g_sliderTarget->EndDraw())) {
        g_sliderBrush.Reset();
        g_sliderTarget.Reset();
        return;
    }
    GdiFlush();
    PaintSliderLens(pixels, width, height, shape);
}

void PaintReceivedText(const DRAWITEMSTRUCT& item, HBITMAP backing) {
    g_textBackdrop = CreatePatternBrush(backing);
    SendMessageW(item.hwndItem, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(item.hDC), PRF_CLIENT);
    if (g_textBackdrop) DeleteObject(g_textBackdrop);
    g_textBackdrop = nullptr;

    RECT area{};
    SendMessageW(item.hwndItem, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&area));
    const HGDIOBJ oldFont = g_uiFont ? SelectObject(item.hDC, g_uiFont) : nullptr;
    TEXTMETRICW metric{};
    GetTextMetricsW(item.hDC, &metric);
    if (oldFont) SelectObject(item.hDC, oldFont);
    const int lineHeight = std::max<int>(1, metric.tmHeight);
    const int visibleLines = std::max<int>(1, (area.bottom - area.top) / lineHeight);
    const int lines = static_cast<int>(SendMessageW(item.hwndItem, EM_GETLINECOUNT, 0, 0));
    if (lines <= visibleLines) return;
    const int first = static_cast<int>(SendMessageW(item.hwndItem, EM_GETFIRSTVISIBLELINE, 0, 0));
    const float scale = GetDpiForWindow(item.hwndItem) / 96.0f;
    const float span = static_cast<float>(area.bottom - area.top);
    const float thumb = std::max(8.0f * scale, span * visibleLines / lines);
    const float y = area.top + (span - thumb) * std::min(1.0f, first / static_cast<float>(lines - visibleLines));
    const float x = item.rcItem.right - 9.0f * scale;
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen ink(Gdiplus::Color(125, 213, 231, 247), 2.5f * scale);
    ink.SetStartCap(Gdiplus::LineCapRound);
    ink.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&ink, x, y + scale, x, y + thumb - scale);
}

void PaintPhoneItem(const DRAWITEMSTRUCT& item) {
    const int width = item.rcItem.right - item.rcItem.left;
    const int height = item.rcItem.bottom - item.rcItem.top;
    if (width < 1 || height < 1) return;
    BITMAPINFO dib{};
    dib.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* data = nullptr;
    const HBITMAP bitmap = CreateDIBSection(item.hDC, &dib, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!bitmap) return;
    const HDC buffer = CreateCompatibleDC(item.hDC);
    if (!buffer) { DeleteObject(bitmap); return; }
    const HGDIOBJ previous = SelectObject(buffer, bitmap);
    auto* pixels = static_cast<uint32_t*>(data);
    DRAWITEMSTRUCT local = item;
    local.hDC = buffer;
    local.rcItem = {0, 0, width, height};
    PaintControlBackdrop(item, pixels, width, height);
    if (item.CtlID == kBallSlider) PaintBallSlider(local, pixels, width, height);
    else if (item.CtlID == kReceivedText) PaintReceivedText(local, bitmap);
    else PaintPhoneControl(local);
    GdiFlush();
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) pixels[i] |= 0xff000000u;
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(item.hDC, item.rcItem.left, item.rcItem.top, width, height,
        buffer, 0, 0, width, height, blend);
    SelectObject(buffer, previous);
    DeleteDC(buffer);
    DeleteObject(bitmap);
}

struct PointerImpact {
    float normal[3]{};
    float strength = 0.0f;
    double start = 0.0;
};
PointerImpact g_impacts[3];
size_t g_nextImpact = 0;
double g_lastHover = 0;
float g_lastHoverNormal[3]{};
bool g_hoveringBall = false;
struct WaterDrop {
    float x = 0, y = 0, vx = 0, vy = 0, radius = 0, life = 0;
    double start = 0;
};
WaterDrop g_drops[16];
size_t g_nextDrop = 0;
int g_clicks = 0;
double g_firstClick = 0, g_lastSplash = 0;

bool PickBallSurface(POINT client, float (&normal)[3]) {
    RECT bounds{};
    GetClientRect(g_wnd, &bounds);
    const float radius = g_opt.radius * GetDpiForWindow(g_wnd) / 96.0f;
    constexpr float eye = 6.0f;  // same pinhole camera as the glass shader
    const float x = (client.x - bounds.right * 0.5f) / radius;
    const float y = (client.y - PhoneBallCenterY(static_cast<float>(bounds.bottom), GetDpiForWindow(g_wnd) / 96.0f)) / radius;
    if (x * x + y * y >= 0.98f * 0.98f) return false;
    const float scale = 1.0f / std::sqrt(x * x + y * y + eye * eye);
    const float dz = eye * scale;
    const float discriminant = 1.0f - eye * eye * (1.0f - dz * dz);
    if (discriminant <= 0.0f) return false;
    const float t = eye * dz - std::sqrt(discriminant);
    normal[0] = t * x * scale;
    normal[1] = t * y * scale;
    normal[2] = t * dz - eye;
    return true;
}

void AddPointerImpact(const float (&normal)[3], float strength, double now) {
    auto& pulse = g_impacts[g_nextImpact];
    memcpy(pulse.normal, normal, sizeof(pulse.normal));
    pulse.strength = strength;
    pulse.start = now;
    g_nextImpact = (g_nextImpact + 1) % ARRAYSIZE(g_impacts);
}

void Splash(const float (&normal)[3], double now) {
    const float outward = std::hypot(normal[0], normal[1]);
    const float direction = outward > 0.12f ? std::atan2(normal[1], normal[0]) : -1.57f;
    for (size_t i = 0; i < 8; ++i) {
        const float jitter = std::sin(static_cast<float>(i) * 2.9f);
        const float angle = direction + (static_cast<float>(i) - 3.5f) * 0.18f + jitter * 0.07f;
        const float speed = 1.40f + 0.90f * (0.5f + 0.5f * jitter);
        auto& drop = g_drops[g_nextDrop];
        const float launch = std::max(outward, 0.64f) * 0.91f;
        drop = {std::cos(angle) * launch, std::sin(angle) * launch,
            std::cos(angle) * speed, std::sin(angle) * speed - 0.20f,
            0.010f + 0.017f * (0.5f + 0.5f * jitter),
            0.42f + 0.13f * (0.5f - 0.5f * jitter), now};
        g_nextDrop = (g_nextDrop + 1) % ARRAYSIZE(g_drops);
    }
}

void TapBall(const float (&normal)[3], double now) {
    AddPointerImpact(normal, 0.081f, now);
    if (now - g_firstClick > 0.38) {
        g_firstClick = now;
        g_clicks = 0;
    }
    if (++g_clicks >= 3 && now - g_lastSplash > 0.32) {
        Splash(normal, now);
        AddPointerImpact(normal, 0.023f, now);
        g_lastSplash = now;
        g_clicks = 0;
    }
}

void SamplePointer(Constants& constants, double now) {
    if (!g_opt.pairPhone || g_reduceMotion || g_showQr) {
        g_hoveringBall = false;
        return;
    }
    POINT pointer{};
    float normal[3]{};
    if (!GetCursorPos(&pointer)) return;
    const HWND hit = WindowFromPoint(pointer);
    if ((hit == g_wnd || hit == g_surface) && ScreenToClient(g_wnd, &pointer) && PickBallSurface(pointer, normal)) {
        const float dx = normal[0] - g_lastHoverNormal[0], dy = normal[1] - g_lastHoverNormal[1];
        if (now - g_lastHover >= 0.095 && (!g_hoveringBall || dx * dx + dy * dy >= 0.075f * 0.075f)) {
            AddPointerImpact(normal, 0.044f, now);
            g_lastHover = now;
            memcpy(g_lastHoverNormal, normal, sizeof(normal));
        }
        g_hoveringBall = true;
    } else {
        g_hoveringBall = false;
    }
    for (size_t i = 0; i < ARRAYSIZE(g_impacts); ++i) {
        const auto& impact = g_impacts[i];
        const float age = static_cast<float>(now - impact.start);
        if (impact.strength <= 0.0f || age >= 1.0f) continue;
        const float damping = (1.0f - age) * (1.0f - age);
        const float attack = std::min(age / 0.055f, 1.0f);
        memcpy(constants.mouseImpacts[i], impact.normal, sizeof(impact.normal));
        constants.mouseImpacts[i][3] = impact.strength * attack * damping;
        constants.mouseFronts[i] = 1.62f * age;
    }
    size_t visible = 0;
    for (const auto& drop : g_drops) {
        const float age = static_cast<float>(now - drop.start);
        if (drop.life <= 0.0f || age >= drop.life) continue;
        auto& particle = constants.waterDrops[visible++];
        particle[0] = drop.x + drop.vx * age;
        particle[1] = drop.y + drop.vy * age + 1.9f * age * age;
        particle[2] = drop.radius * (1.0f - 0.28f * age / drop.life);
        particle[3] = std::min(age / 0.04f, 1.0f) * std::clamp((drop.life - age) / 0.16f, 0.0f, 1.0f);
    }
    constants.splashCount = static_cast<float>(visible);
}
#endif

class App {
public:
    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_))) return false;
        ComPtr<IDXGIDevice> dxgi; dev_.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory_));
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(g_surface ? g_surface : g_wnd, TRUE, &target_))
            || FAILED(dcomp_->CreateVisual(&visual_))) return false;
        target_->SetRoot(visual_.Get());
        if (FAILED(dev_->CreateVertexShader(g_BubbleVS, sizeof(g_BubbleVS), nullptr, &vs_)) ||
            FAILED(dev_->CreatePixelShader(g_BubblePS, sizeof(g_BubblePS), nullptr, &ps_)) ||
            FAILED(dev_->CreatePixelShader(g_BubbleRaysPS, sizeof(g_BubbleRaysPS), nullptr, &psRays_))) return false;
        if (g_opt.pcPreview && FAILED(dev_->CreatePixelShader(g_PcPairingPS, sizeof(g_PcPairingPS),
            nullptr, &psPc_))) return false;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(Constants); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&cb, nullptr, &cb_))) return false;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &sampler_);
        if (g_opt.mode == 1 && !LoadImage(g_opt.image)) g_opt.mode = 2;
        if (g_opt.mode == 0) {
            auto& sd2 = docky::desk::SharedDesktop::Get();
            sd2.AddUser(); deskUser_ = true;
            sd2.SetOwner(dev_.Get());
        }
        start_ = Now();
        return true;
    }
    void Shutdown() {
        if (deskUser_) { auto& sd = docky::desk::SharedDesktop::Get(); sd.SetOwner(nullptr); sd.RemoveUser(); }
        if (visual_) { visual_->SetContent(nullptr); dcomp_->Commit(); }
    }
    bool Presented() const { return lastFrame_ > 0; }

    void Frame() {
        const double now = Now();
#ifdef DOCKY_PHONE_PAIRING
        if (g_pcOffer) {
            PollPcBall(now);
            if (g_quit) return;
        } else if (g_opt.pcPreview && g_pcStarted > 0) TickPcPreview(now);
#endif
        RECT cr{}; GetClientRect(g_wnd, &cr);
        const UINT w = static_cast<UINT>(std::max<LONG>(cr.right, 1)), h = static_cast<UINT>(std::max<LONG>(cr.bottom, 1));
        if (!EnsureSwap(w, h)) return;
        const float dpi = static_cast<float>(GetDpiForWindow(g_wnd)) / 96.f;
        if (g_opt.mode == 0) UpdateDesktop();
        if (g_opt.demoProgress > 0 && g_finishedAt < 0)
            g_transferProgress = static_cast<float>(std::clamp((now - start_) / g_opt.demoProgress, 0.001, 0.995));
        const bool snapshotting = !g_opt.seqDir.empty() && seqIndex_ < g_opt.seqCount;

        Constants c{};
        c.targetSize[0] = static_cast<float>(w); c.targetSize[1] = static_cast<float>(h);
        c.center[0] = w * 0.5f; c.center[1] = h * 0.5f;
#ifdef DOCKY_PHONE_PAIRING
        if (g_opt.pairPhone) {
            c.center[1] = PhoneBallCenterY(static_cast<float>(h), dpi);
            if (now - g_pressAt < 0.16 && !g_reduceMotion && IsWindowVisible(g_slider)) {
                InvalidateRect(g_slider, nullptr, FALSE);
            }
        }
#endif
        c.radius = g_opt.radius * dpi;
        c.time = static_cast<float>(now - start_);
        c.mode = static_cast<float>(g_opt.mode == 0 && !deskSrv_ ? 2 : g_opt.mode);
        c.opaque = (g_opt.mode != 0 || snapshotting) ? 1.f : 0.f;
#ifdef DOCKY_PHONE_PAIRING
        if (IsPcReceiveBall()) {
            c.opaque = 0.f;
            c.radius = std::min(w, h) * kPcReceiveRadiusRatio;
            if (g_reduceMotion) c.time = 0.f;
        }
#endif
        POINT org{0, 0}; ClientToScreen(g_wnd, &org);
        c.deskOrigin[0] = static_cast<float>(org.x - monitor_.left); c.deskOrigin[1] = static_cast<float>(org.y - monitor_.top);
        c.deskSize[0] = static_cast<float>(deskW_); c.deskSize[1] = static_cast<float>(deskH_);
        if (g_opt.mode == 1) {  // cover the window, keeping the picture's aspect
            const float wa = static_cast<float>(w) / h, ia = static_cast<float>(imgW_) / imgH_;
            c.imgScale[0] = wa > ia ? 1.f : wa / ia; c.imgScale[1] = wa > ia ? ia / wa : 1.f;
            c.imgOffset[0] = (1 - c.imgScale[0]) * 0.5f + static_cast<float>(g_opt.scroll * (now - start_) / imgW_);
            c.imgOffset[1] = (1 - c.imgScale[1]) * 0.5f;
        }
        c.dpi = dpi; c.showBubbles = g_opt.bubbles ? 1.f : 0.f; c.rippleAmp = g_opt.ripple;
        c.codeAmp = g_opt.codeAmp;
        c.phonePanel = g_opt.pairPhone ? 1.0f : 0.0f;
        c.sinkAge = static_cast<float>(now - g_sinkAt);
        const double finishStart = std::max(g_finishedAt, g_connectedAt + 0.98) + 0.20;
        c.finishAge = g_finishedAt < 0 ? -1.0f
            : static_cast<float>(now - (g_reduceMotion ? g_finishedAt : finishStart));
        if (g_transferProgress > 0.0f) {
            const double dt = progressPaintAt_ > 0 ? std::clamp(now - progressPaintAt_, 0.0, 0.050) : 0.016;
            const float step = g_reduceMotion ? 1.0f : static_cast<float>(1.0 - std::exp(-dt / 0.055));
            rimProgress_ = std::lerp(rimProgress_, g_transferProgress, step);
            c.rimProgress = g_finishedAt >= 0 && now >= finishStart - 0.04 ? 1.0f : rimProgress_;
            c.rimOpacity = g_reduceMotion && g_finishedAt >= 0 ? 0.0f
                : 1.0f - std::clamp(c.finishAge / 0.24f, 0.0f, 1.0f);
        } else {
            rimProgress_ = 0.0f;
        }
        progressPaintAt_ = now;
#ifdef DOCKY_PHONE_PAIRING
        c.fileCount = static_cast<float>(g_outgoing.FileCount());
#endif
        c.reducedMotion = g_reduceMotion ? 1.0f : 0.0f;
#ifdef DOCKY_PHONE_PAIRING
        if (g_opt.pcPreview) {
            const auto layout = docky::pairing::LayoutPcBubbles(static_cast<float>(w), static_cast<float>(h));
            c.center[0] = layout.local.x;
            c.center[1] = layout.local.y;
            const float merge = g_pcMotion.MergeAmount(now, g_reduceMotion);
            c.radius = layout.local.radius * (1.0f + 0.065f * std::sin(merge * 3.14159265f));
            c.phonePanel = 1.0f;
            c.rippleAmp = g_reduceMotion ? 0.0f : 0.010f;
            for (int i = 0; i < docky::pairing::kPcPeerCount; ++i)
                c.pcPeers[i] = g_pcMotion.Bubble(i, layout, now, g_reduceMotion);
            if (g_reduceMotion) c.time = 0.0f;
        }
#endif
        if (g_connectedAt > 0) {
            // Advance only when presenting frames, so a compositor stall cannot skip the wave.
            successAge_ = successPaintAt_ > 0
                ? std::min(1.0, successAge_ + std::clamp(now - successPaintAt_, 0.0, 0.050)) : 0.001;
            successPaintAt_ = now;
            c.successAge = static_cast<float>(successAge_);
            if (g_opt.codeMode == 4 && successAge_ >= (g_reduceMotion ? 0.18 : 0.98)) {
                g_opt.codeMode = 0;
                ZeroMemory(g_opt.codeBits, sizeof(g_opt.codeBits));
            }
        } else {
            successAge_ = 0;
            successPaintAt_ = 0;
        }
        c.codeMode = static_cast<float>(g_opt.codeMode);
#ifdef DOCKY_PHONE_PAIRING
        if (g_showQr && g_qrSize > 0) {
            c.codeMode = 0;
            c.qrSize = static_cast<uint32_t>(g_qrSize);
            memcpy(c.qrBits, g_qr, sizeof(c.qrBits));
        }
        SamplePointer(c, now);
#endif
        memcpy(c.codeBits, g_opt.codeBits, sizeof(c.codeBits));

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); ctx_->Unmap(cb_.Get(), 0); }
        ComPtr<ID3D11Texture2D> back; swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> rtv; dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        const float clear[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* r = rtv.Get();
        ctx_->OMSetRenderTargets(1, &r, nullptr);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cbs[] = {cb_.Get()};
        ctx_->VSSetConstantBuffers(0, 1, cbs); ctx_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* ss[] = {sampler_.Get()};
        ctx_->PSSetSamplers(0, 1, ss);
        ID3D11ShaderResourceView* srv = g_opt.mode == 1 ? imgSrv_.Get() : deskSrv_.Get();
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(g_opt.pcPreview ? psPc_.Get() : ps_.Get(), nullptr, 0);
        if (c.codeMode > 3.5f && EnsurePass1(w, h)) {
            // the ball into an offscreen picture, then the rays over it (they need the glass beside each beam)
            const float zero[4] = {0, 0, 0, 0};
            ctx_->ClearRenderTargetView(pass1Rtv_.Get(), zero);
            ID3D11RenderTargetView* p1 = pass1Rtv_.Get();
            ctx_->OMSetRenderTargets(1, &p1, nullptr);
            ctx_->Draw(3, 0);
            ctx_->OMSetRenderTargets(1, &r, nullptr);
            ID3D11ShaderResourceView* s1 = pass1Srv_.Get();
            ctx_->PSSetShaderResources(1, 1, &s1);
            ctx_->PSSetShader(psRays_.Get(), nullptr, 0);
            ctx_->Draw(3, 0);
            ID3D11ShaderResourceView* n1 = nullptr; ctx_->PSSetShaderResources(1, 1, &n1);
        } else {
            ctx_->Draw(3, 0);
        }
        ID3D11ShaderResourceView* none = nullptr; ctx_->PSSetShaderResources(0, 1, &none);
#ifdef DOCKY_PHONE_PAIRING
        if (g_pcOffer) {
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            PaintPcBallText(back.Get(), c);
        }
#endif

        if (snapshotting && (!g_opt.pcPreview || g_pcStarted > 0.0) &&
            now - start_ >= g_opt.seqAfter + seqIndex_ * g_opt.seqInterval) {
            wchar_t name[64]; swprintf_s(name, L"\\bubble_%04d.bmp", seqIndex_++);
            SaveBmp(back.Get(), g_opt.seqDir + name);
        }
        if (FAILED(swap_->Present(0, 0))) return;
        dcomp_->Commit();
        const double t1 = Now();
#ifdef DOCKY_PHONE_PAIRING
        if (g_opt.pcPreview && g_pcStarted == 0.0) {
            // Time discovery from the first presented frame, after driver
            // shader warm-up, so startup cannot skip the small bubbles.
            start_ = g_pcStarted = t1;
        }
#endif
        if (lastFrame_ > 0) frameIv_.push_back(t1 - lastFrame_);
        lastFrame_ = t1;
        if (g_opt.exitAfter > 0 && now - start_ > g_opt.exitAfter) { WriteBench(w, h); g_quit = true; }
    }

private:
#ifdef DOCKY_PHONE_PAIRING
    void PaintPcBallText(ID3D11Texture2D* back, const Constants& c) {
        const auto& offer = *g_pcOffer;
        const float uiScale = offer.sending ? c.dpi :
            std::min(c.dpi, std::min(c.targetSize[0], c.targetSize[1]) / kPcReceiveWindowDip);
        if (!pcInk_) {
            ComPtr<IDXGISurface> surface;
            if (FAILED(back->QueryInterface(IID_PPV_ARGS(&surface))) ||
                (!pcTextFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, pcTextFactory_.GetAddressOf()))) ||
                (!pcFontFactory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(pcFontFactory_.GetAddressOf()))))) return;
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (FAILED(pcTextFactory_->CreateDxgiSurfaceRenderTarget(surface.Get(),
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, format, 96, 96), &pcInk_))) return;
            const auto make = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& text,
                                  bool keepExtension = false) {
                if (FAILED(pcFontFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size * uiScale, L"", &text))) return false;
                text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, keepExtension ? static_cast<UINT32>(L'.') : 0u,
                    keepExtension ? 1u : 0u};
                ComPtr<IDWriteInlineObject> sign;
                if (SUCCEEDED(pcFontFactory_->CreateEllipsisTrimmingSign(text.Get(), &sign))) text->SetTrimming(&trim, sign.Get());
                return true;
            };
            if (!make(offer.sending ? 16.f : 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, pcTitle_) ||
                !make(offer.sending ? 13.f : 14.f, DWRITE_FONT_WEIGHT_MEDIUM, pcDetail_) ||
                (!offer.sending && !make(14.f, DWRITE_FONT_WEIGHT_MEDIUM, pcFilename_, true)) ||
                FAILED(pcInk_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &pcBrush_))) {
                pcInk_.Reset(); return;
            }
            pcInk_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        const float x = c.center[0], y = c.center[1], radius = c.radius;
        const float lineH = 23 * uiScale;
        const auto line = [&](const std::wstring& text, float cy, float width, bool emph,
                              bool action = false, bool filename = false) {
            const auto rect = D2D1::RectF(x - width * 0.5f, cy - lineH * 0.5f, x + width * 0.5f, cy + lineH * 0.5f);
            const auto shadow = D2D1::RectF(rect.left, rect.top + uiScale, rect.right, rect.bottom + uiScale);
            const auto font = filename ? pcFilename_.Get() : emph ? pcTitle_.Get() : pcDetail_.Get();
            pcBrush_->SetColor(D2D1::ColorF(0.01f, 0.025f, 0.05f, 0.87f));
            pcInk_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), font, shadow, pcBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            pcBrush_->SetColor(action ? D2D1::ColorF(0.78f, 0.91f, 1.f) :
                D2D1::ColorF(0.96f, 0.98f, 1.f, emph ? 1.f : 0.92f));
            pcInk_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), font, rect, pcBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        pcInk_->BeginDraw();
        if (offer.sending) {
            const auto status = offer.failed ? Ui(offer.declined ? L"对方已拒绝" : L"传输已中断") :
                Ui(g_finishedAt >= 0 ? L"已发送 · " : offer.accepted ? L"正在发送 · " : L"等待对方确认 · ");
            line(offer.failed ? std::wstring(status) : std::wstring(status) + offer.sender,
                31 * c.dpi, c.targetSize[0] - 35 * c.dpi, true);
            if (!offer.accepted && !offer.failed) {
                const auto layout = docky::pairing::LayoutPcBubbles(c.targetSize[0], c.targetSize[1]);
                line(offer.sender, layout.peers[kLivePeer].y + layout.peers[kLivePeer].radius + lineH,
                    c.targetSize[0] * 0.70f, false);
            } else if (offer.accepted && !offer.Terminal() && g_pcMotion.MergeAmount(Now(), g_reduceMotion) >= 1.0f) {
                line(std::to_wstring(offer.percent) + L"%", y, radius, true);
            }
        } else if (offer.complete) {
            line(Ui(L"接收完成"), y - radius * 0.41f, radius * 1.40f, true);
            line(Ui(L"打开文件夹"), y + radius * 0.49f, radius * 1.40f, false, true);
        } else if (offer.failed) {
            line(Ui(L"传输已中断"), y, radius * 1.40f, true);
        } else if (offer.accepted) {
            line(Ui(offer.percent >= 100 ? L"正在校验文件…" : L"正在接收…"), y - radius * 0.17f, radius * 1.50f, true);
            line(offer.file, y + radius * 0.09f, radius * 1.62f, false, false, true);
        } else {
            POINT pointer{};
            const bool overWindow = GetCursorPos(&pointer) && WindowFromPoint(pointer) == g_wnd;
            if (overWindow) ScreenToClient(g_wnd, &pointer);
            g_pcReceiveCloseVisible = overWindow && std::hypot(pointer.x - x, pointer.y - y) <= radius * 1.03f;
            const auto hot = overWindow ? PcReceiveActionAt(g_wnd, pointer) : PcReceiveAction::None;
            line(offer.sender, y - radius * 0.45f, radius * 1.46f, true);
            line(Ui(L"想给你发送文件"), y - radius * 0.24f, radius * 1.53f, false);
            line(offer.file, y - radius * 0.02f, radius * 1.60f, false, false, true);
            line(offer.bytes, y + radius * 0.18f, radius * 1.34f, false);
            const auto pill = PcReceiveControl(PcReceiveAction::Confirm, x, y, radius);
            pcBrush_->SetColor(D2D1::ColorF(0.03f, 0.10f, 0.18f, hot == PcReceiveAction::Confirm ? 0.84f : 0.66f));
            pcInk_->FillRoundedRectangle(D2D1::RoundedRect(pill, lineH, lineH), pcBrush_.Get());
            line(Ui(L"确认接收"), (pill.top + pill.bottom) * 0.5f, pill.right - pill.left, true, true);
            if (g_pcReceiveCloseVisible) {
                const auto close = PcReceiveControl(PcReceiveAction::Close, x, y, radius);
                const auto center = D2D1::Point2F((close.left + close.right) * 0.5f, (close.top + close.bottom) * 0.5f);
                const float size = (close.right - close.left) * 0.5f, mark = size * 0.33f;
                pcBrush_->SetColor(D2D1::ColorF(0.02f, 0.04f, 0.08f, hot == PcReceiveAction::Close ? 0.86f : 0.68f));
                pcInk_->FillEllipse(D2D1::Ellipse(center, size, size), pcBrush_.Get());
                pcBrush_->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.f));
                pcInk_->DrawLine(D2D1::Point2F(center.x - mark, center.y - mark),
                    D2D1::Point2F(center.x + mark, center.y + mark), pcBrush_.Get(), 1.75f * c.dpi);
                pcInk_->DrawLine(D2D1::Point2F(center.x + mark, center.y - mark),
                    D2D1::Point2F(center.x - mark, center.y + mark), pcBrush_.Get(), 1.75f * c.dpi);
            }
        }
        if (FAILED(pcInk_->EndDraw())) pcInk_.Reset();
    }
#endif
    bool LoadImage(const std::wstring& path) {
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
            FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        UINT iw = 0, ih = 0; conv->GetSize(&iw, &ih);
        std::vector<uint8_t> px(static_cast<size_t>(iw) * ih * 4);
        if (FAILED(conv->CopyPixels(nullptr, iw * 4, static_cast<UINT>(px.size()), px.data()))) return false;
        D3D11_TEXTURE2D_DESC d{}; d.Width = iw; d.Height = ih; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex)) || FAILED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &imgSrv_))) return false;
        ctx_->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), iw * 4, 0);
        ctx_->GenerateMips(imgSrv_.Get());
        imgW_ = iw; imgH_ = ih;
        return true;
    }

    void UpdateDesktop() {
        if (!deskUser_) return;
        HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
        monitor_ = mi.rcMonitor;
        auto& sd = docky::desk::SharedDesktop::Get();
        sd.Want(monitor_);
        if (sd.Owned()) sd.PumpOwned(0);
        if (!sd.Valid()) return;
        if (!view_ || viewGen_ != sd.Generation()) {
            view_.Reset();
            if (HANDLE hnd = sd.SharedHandle()) dev_->OpenSharedResource(hnd, IID_PPV_ARGS(&view_));
            viewGen_ = sd.Generation(); lastSerial_ = 0;
        }
        const uint64_t serial = sd.Serial();
        if (!view_ || serial == lastSerial_) return;
        lastSerial_ = serial;
        D3D11_TEXTURE2D_DESC vd{}; view_->GetDesc(&vd);
        if (!desk_ || vd.Width != deskW_ || vd.Height != deskH_) {
            desk_.Reset(); deskSrv_.Reset();
            D3D11_TEXTURE2D_DESC d{}; d.Width = vd.Width; d.Height = vd.Height; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &desk_)) || FAILED(dev_->CreateShaderResourceView(desk_.Get(), nullptr, &deskSrv_))) { desk_.Reset(); deskSrv_.Reset(); return; }
            deskW_ = vd.Width; deskH_ = vd.Height;
        }
        const D3D11_BOX box{0, 0, 0, vd.Width, vd.Height, 1};
        ctx_->CopySubresourceRegion(desk_.Get(), 0, 0, 0, 0, view_.Get(), 0, &box);
        ctx_->GenerateMips(deskSrv_.Get());
    }

    bool EnsurePass1(UINT w, UINT h) {
        if (pass1_ && w == pass1W_ && h == pass1H_) return true;
        pass1_.Reset(); pass1Rtv_.Reset(); pass1Srv_.Reset();
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &pass1_)) || FAILED(dev_->CreateRenderTargetView(pass1_.Get(), nullptr, &pass1Rtv_)) ||
            FAILED(dev_->CreateShaderResourceView(pass1_.Get(), nullptr, &pass1Srv_))) { pass1_.Reset(); return false; }
        pass1W_ = w; pass1H_ = h;
        return true;
    }

    bool EnsureSwap(UINT w, UINT h) {
        if (swap_ && w == swapW_ && h == swapH_) return true;
#ifdef DOCKY_PHONE_PAIRING
        pcBrush_.Reset();
        pcInk_.Reset();
#endif
        if (swap_) { ctx_->ClearState(); ctx_->Flush(); if (SUCCEEDED(swap_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) { swapW_ = w; swapH_ = h; return true; } swap_.Reset(); }
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory_->CreateSwapChainForComposition(dev_.Get(), &d, nullptr, &swap_))) return false;
        visual_->SetContent(swap_.Get());
        swapW_ = w; swapH_ = h;
        return true;
    }

    bool SaveBmp(ID3D11Texture2D* tex, const std::wstring& path) {
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
        ctx_->CopyResource(st.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) { ctx_->Unmap(st.Get(), 0); return false; }
        const uint32_t row = d.Width * 4, size = row * d.Height;
        BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + size;
        BITMAPINFOHEADER ih{}; ih.biSize = sizeof(ih); ih.biWidth = static_cast<LONG>(d.Width); ih.biHeight = -static_cast<LONG>(d.Height); ih.biPlanes = 1; ih.biBitCount = 32; ih.biSizeImage = size;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        for (UINT y = 0; y < d.Height; ++y) fwrite(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, row, 1, f);
        fclose(f);
        ctx_->Unmap(st.Get(), 0);
        return true;
    }

    void WriteBench(UINT w, UINT h) {
        if (g_opt.benchOut.empty() || frameIv_.size() < 10) return;
        std::vector<double> v(frameIv_.begin() + static_cast<long long>(frameIv_.size() / 10), frameIv_.end());
        std::sort(v.begin(), v.end());
        double mean = 0; for (double x : v) mean += x; mean /= v.size();
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_opt.benchOut.c_str(), L"w") != 0 || !f) return;
        fprintf(f, "%ux%u px, radius %.0f DIP: %zu frames, %.1f fps, interval mean %.3f ms, p99 %.3f ms, max %.3f ms\n", w, h, g_opt.radius, v.size(), 1.0 / mean, mean * 1000,
                v[static_cast<size_t>(v.size() * 0.99)] * 1000, v.back() * 1000);
        fclose(f);
    }

    ComPtr<ID3D11Device> dev_; ComPtr<ID3D11DeviceContext> ctx_; ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDCompositionDevice> dcomp_; ComPtr<IDCompositionTarget> target_; ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDXGISwapChain1> swap_; UINT swapW_ = 0, swapH_ = 0;
    ComPtr<ID3D11VertexShader> vs_; ComPtr<ID3D11PixelShader> ps_, psRays_, psPc_;
    ComPtr<ID3D11Texture2D> pass1_; ComPtr<ID3D11RenderTargetView> pass1Rtv_; ComPtr<ID3D11ShaderResourceView> pass1Srv_; UINT pass1W_ = 0, pass1H_ = 0;
    ComPtr<ID3D11Buffer> cb_; ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11ShaderResourceView> deskSrv_, imgSrv_; UINT imgW_ = 1, imgH_ = 1;
    ComPtr<ID3D11Texture2D> view_, desk_; UINT deskW_ = 1, deskH_ = 1; uint64_t lastSerial_ = 0; unsigned viewGen_ = ~0u;
    RECT monitor_{};
    bool deskUser_ = false;
    double start_ = 0, lastFrame_ = 0;
    double successPaintAt_ = 0, successAge_ = 0;
    double progressPaintAt_ = 0.0;
    float rimProgress_ = 0.0f;
    int seqIndex_ = 0;
    std::vector<double> frameIv_;
#ifdef DOCKY_PHONE_PAIRING
    ComPtr<ID2D1Factory> pcTextFactory_;
    ComPtr<IDWriteFactory> pcFontFactory_;
    ComPtr<ID2D1RenderTarget> pcInk_;
    ComPtr<ID2D1SolidColorBrush> pcBrush_;
    ComPtr<IDWriteTextFormat> pcTitle_, pcDetail_, pcFilename_;
#endif
};

App* g_app = nullptr;

#ifdef DOCKY_PHONE_PAIRING
LRESULT CALLBACK SurfaceProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
#endif

#ifdef DOCKY_PHONE_PAIRING
bool QueueFiles(const std::vector<std::wstring>& paths) {
    if (g_outgoing.SetFiles(paths)) {
        g_sinkAt = Now();
        const float contact[3] = {0.0f, 0.35f, -0.937f};
        AddPointerImpact(contact, 0.07f, g_sinkAt);
        SetWindowTextW(g_sendFiles, (Ui(L"已选 ") + std::to_wstring(g_outgoing.FileCount()) + Ui(L" 个文件")).c_str());
        return true;
    } else {
        SetReceiveStatus(Ui(L"文件无法读取，或超过 128 MB／64 个"));
        return false;
    }
}

void QueueManualFiles(const std::vector<std::wstring>& paths) {
    if (QueueFiles(paths)) {
        g_manualFiles = true;
        g_widgetDraft.files.clear();
    }
}

void LoadWidgetDraft() {
    if (!g_opt.pairPhone || g_transferring || g_connectedAt > 0 || !g_sendFiles) return;
    docky::pairing::WidgetDraft draft;
    if (!docky::pairing::WidgetQueue().Read(draft)) return;
    if (!g_manualFiles && draft.files != g_widgetDraft.files) {
        if (draft.files.empty()) {
            g_outgoing.SetFiles({});
            g_widgetDraft.files.clear();
            SetWindowTextW(g_sendFiles, Ui(L"选择发送文件"));
        } else if (QueueFiles(draft.files)) {
            g_widgetDraft.files = std::move(draft.files);
        }
    }
    if (!g_manualText && draft.texts != g_widgetDraft.texts && g_outgoing.SetText(draft.JoinedText())) {
        g_widgetDraft.texts = std::move(draft.texts);
        SetWindowTextW(g_sendText, Ui(g_widgetDraft.texts.empty() ? L"加入剪贴板文字" : L"文字已加入"));
    }
}
#endif

#ifdef DOCKY_PHONE_PAIRING
// Ask before Back interrupts a transfer. The prompt takes the received-text field and action row, in the
// same glass, and gives them back afterwards.
void RevealAction(int slot) {
    HWND displaced[3] = {g_receivedText, g_copyText, g_openFolder};
    if (g_confirming) g_hiddenForConfirm[slot] = true;
    else ShowWindow(displaced[slot], SW_SHOW);
}

void ShowBackConfirm(bool show, bool closing) {
    if (show) {
        g_confirmClose = closing;
        SetWindowTextW(g_confirmText, closing ? Ui(L"关闭会中断当前传输") : Ui(L"返回会中断当前传输"));
        SetWindowTextW(g_confirmLeave, closing ? Ui(L"中断并关闭") : Ui(L"中断并返回"));
        InvalidateRect(g_confirmText, nullptr, FALSE);
        InvalidateRect(g_confirmLeave, nullptr, FALSE);
    }
    if (show == g_confirming) return;
    g_confirming = show;
    HWND displaced[3] = {g_receivedText, g_copyText, g_openFolder};
    for (int i = 0; i < 3; ++i) {
        if (show) {
            g_hiddenForConfirm[i] = IsWindowVisible(displaced[i]) != FALSE;
            ShowWindow(displaced[i], SW_HIDE);
        } else if (g_hiddenForConfirm[i]) {
            ShowWindow(displaced[i], SW_SHOW);
            g_hiddenForConfirm[i] = false;
        }
    }
    for (HWND prompt : {g_confirmText, g_confirmStay, g_confirmLeave}) ShowWindow(prompt, show ? SW_SHOW : SW_HIDE);
    LayoutPhoneControls(g_wnd);
    SetFocus(show ? g_confirmStay : (g_confirmClose ? g_close : g_back));
}

// Back: drop the current phone session (cancelling any transfer) and start over with a fresh code.
void ResetPairing() {
    g_phoneHost.reset();
    g_incoming.Cancel();
    g_outgoing.Clear();
    g_widgetDraft = {};
    g_manualFiles = g_manualText = false;
    g_activePhone.clear();
    g_targetPhone.clear();
    g_receivePrompt.clear();
    g_transferring = false;
    g_confirming = false;
    for (bool& hidden : g_hiddenForConfirm) hidden = false;
    for (HWND prompt : {g_confirmText, g_confirmStay, g_confirmLeave}) ShowWindow(prompt, SW_HIDE);
    g_connectedAt = 0;
    g_finishedAt = -100;
    g_sinkAt = -100;
    g_transferProgress = 0.0f;
    g_filesReceived = 0;
    g_lastText.clear();
    SetWindowTextW(g_receivedText, L"");
    g_opt.codeMode = 0;
    ZeroMemory(g_opt.codeBits, sizeof(g_opt.codeBits));
    ZeroMemory(g_qr, sizeof(g_qr));
    g_qrSize = 0;
    g_showQr = false;
    if (GetFocus() == g_back) SetFocus(g_wnd);
    for (HWND hide : {g_back, g_receivedText, g_copyText, g_openFolder}) ShowWindow(hide, SW_HIDE);
    for (HWND show : {g_guide, g_qrButton, g_receiveSettings, g_sendFiles, g_sendText}) ShowWindow(show, SW_SHOW);
    SetWindowTextW(g_sendFiles, Ui(L"选择发送文件"));
    SetWindowTextW(g_sendText, Ui(L"加入剪贴板文字"));
    SetReceiveStatus(Ui(L"正在准备手机配对…"));
    UpdateQr();
    LayoutPhoneControls(g_wnd);
    g_phoneHost = docky::pairing::StartPhoneHost(g_wnd, ReceivePhoneMessage);
}
void PrepareShellSend(HWND owner, const std::wstring& path) {
    using namespace docky::pairing;
    const auto phones = ReadTrustedPhones();
    std::wstring target;
    if (phones.size() == 1) {
        target = phones.front().id;
    } else if (!phones.empty()) {
        std::vector<TASKDIALOG_BUTTON> buttons;
        for (size_t i = 0; i < phones.size(); ++i)
            buttons.push_back({static_cast<int>(200 + i), phones[i].name.c_str()});
        buttons.push_back({206, Ui(L"扫描以连接另一台手机")});
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = owner;
        dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        dialog.pszWindowTitle = L"DockDrop";
        dialog.pszMainInstruction = Ui(L"选择接收文件的手机");
        dialog.cButtons = static_cast<UINT>(buttons.size());
        dialog.pButtons = buttons.data();
        int choice = 0;
        if (FAILED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr)) || choice == IDCANCEL) return;
        if (choice >= 200 && static_cast<size_t>(choice - 200) < phones.size())
            target = phones[choice - 200].id;
    }
    if (g_transferring) return;
    if (g_connectedAt > 0) ResetPairing();
    g_targetPhone = std::move(target);
    QueueManualFiles({path});
    ReplyPhone(L"trust-refresh");
}
#endif

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
#ifdef DOCKY_PHONE_PAIRING
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (g_opt.pairPhone && data && data->dwData == docky::pairing::kLoadWidgetDraft && data->cbData == 0)
            return PostMessageW(h, kWidgetDraftReady, 0, 0);
        if (!g_opt.pairPhone || g_transferring || !IsWindowEnabled(h) || !g_shellSendPath.empty()
            || !data || data->dwData != kShellSendFile || !data->lpData
            || data->cbData < sizeof(wchar_t) * 2 || data->cbData > sizeof(wchar_t) * 32768
            || data->cbData % sizeof(wchar_t) != 0) return FALSE;
        const std::wstring_view path(static_cast<const wchar_t*>(data->lpData),
            data->cbData / sizeof(wchar_t));
        if (path.find(L'\0') != path.size() - 1) return FALSE;
        g_shellSendPath.assign(path.data(), path.size() - 1);
        if (PostMessageW(h, kShellSendReady, 0, 0)) return TRUE;
        g_shellSendPath.clear();
        return FALSE;
    }
    case kWidgetDraftReady:
        LoadWidgetDraft();
        return 0;
    case kShellSendReady: {
        const auto path = std::exchange(g_shellSendPath, {});
        if (!path.empty() && !g_transferring) PrepareShellSend(h, path);
        return 0;
    }
    case kPhoneOffer: {
        const auto host = g_phoneHost;
        const auto id = std::exchange(g_receivePrompt, {});
        const auto phones = docky::pairing::ReadTrustedPhones();
        const auto peer = std::find_if(phones.begin(), phones.end(), [&id](const auto& phone) { return phone.id == id; });
        bool accepted = false;
        if (peer != phones.end() && id == g_activePhone && g_connectedAt > 0) {
            const std::wstring text = std::wstring(Ui(g_offerSending ? L"将选中的文件或文字发送到这台手机？"
                                                                    : L"允许这台手机发送文件或文字？"))
                + L"\n\n" + peer->name;
            TASKDIALOGCONFIG dialog{};
            dialog.cbSize = sizeof(dialog);
            dialog.hwndParent = h;
            dialog.dwFlags = TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
            dialog.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
            dialog.pszWindowTitle = L"DockDrop";
            dialog.pszMainInstruction = text.c_str();
            dialog.nDefaultButton = IDNO;
            dialog.pfCallback = [](HWND window, UINT event, WPARAM elapsed, LPARAM, LONG_PTR) -> HRESULT {
                if (event == TDN_TIMER && elapsed >= 38000) SendMessageW(window, TDM_CLICK_BUTTON, IDNO, 0);
                return S_OK;
            };
            int choice = IDNO;
            accepted = SUCCEEDED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr)) && choice == IDYES;
        }
        docky::pairing::ReplyToPhoneHost(host, accepted ? L"ok" : L"error");
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wp);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::vector<std::wstring> paths;
        if (!g_connectedAt && count <= 64) {
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), length + 1));
                paths.push_back(std::move(path));
            }
        }
        DragFinish(drop);
        if (!paths.empty()) QueueManualFiles(paths);
        else if (!g_connectedAt && count > 64) SetReceiveStatus(Ui(L"文件无法读取，或超过 128 MB／64 个"));
        return 0;
    }
    case WM_HSCROLL:
        if (g_slider && reinterpret_cast<HWND>(lp) == g_slider) {
            ChangeRadius(static_cast<int>(SendMessageW(g_slider, TBM_GETPOS, 0, 0)));
            InvalidateRect(g_slider, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (g_receivedText && reinterpret_cast<HWND>(lp) == g_receivedText) {
            HIGHCONTRASTW contrast{sizeof(contrast)};
            if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                (contrast.dwFlags & HCF_HIGHCONTRASTON)) break;
            const auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, RGB(233, 237, 245));
            constexpr COLORREF background = RGB(28, 33, 42);
            SetBkColor(dc, background);
            if (g_textBackdrop) {
                SetBkMode(dc, TRANSPARENT);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                return reinterpret_cast<LRESULT>(g_textBackdrop);
            }
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_COMMAND:
        if (g_opt.pcPreview && HIWORD(wp) == BN_CLICKED) {
            const UINT id = LOWORD(wp);
            if (id == kClose) g_quit = true;
            else if (id == kPcDisconnect) DisconnectDemoPeer(Now());
            else if (id >= kPcFirstPeer && id < kPcFirstPeer + docky::pairing::kPcPeerCount)
                RequestDemoConnection(static_cast<int>(id - kPcFirstPeer), Now());
            return 0;
        }
        if (g_opt.pairPhone && HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == kClose) {
                if (g_transferring && g_finishedAt < 0) ShowBackConfirm(true, true);
                else g_quit = true;
                return 0;
            }
            if (LOWORD(wp) == kBack && g_connectedAt > 0) {
                if (g_transferring && g_finishedAt < 0) ShowBackConfirm(true);
                else { ResetPairing(); LoadWidgetDraft(); }
                return 0;
            }
            if (LOWORD(wp) == kConfirmStay) { ShowBackConfirm(false); return 0; }
            if (LOWORD(wp) == kConfirmLeave) {
                const bool closing = g_confirmClose;
                ShowBackConfirm(false);
                if (closing) g_quit = true;
                else { ResetPairing(); LoadWidgetDraft(); }
                return 0;
            }
            if (LOWORD(wp) == kQrToggle) { g_showQr = !g_showQr; UpdateQr(); return 0; }
            if (LOWORD(wp) == kReceiveSettings && !g_transferring) {
                ShowPhoneSettings(h);
                return 0;
            }
            if (LOWORD(wp) == kCopyText) { CopyReceivedText(); return 0; }
            if (LOWORD(wp) == kSendFiles && g_connectedAt == 0) {
                const auto paths = docky::pairing::ChooseSendFiles(h);
                if (!paths.empty() && g_connectedAt == 0) {
                    QueueManualFiles(paths);
                }
                return 0;
            }
            if (LOWORD(wp) == kSendText && g_connectedAt == 0) {
                const auto text = docky::pairing::ReadClipboardText(h);
                if (!text.empty() && g_outgoing.SetText(text)) {
                    g_manualText = true;
                    g_widgetDraft.texts.clear();
                    SetWindowTextW(g_sendText, Ui(L"文字已加入"));
                }
                else SetReceiveStatus(Ui(L"剪贴板没有可发送的文字"));
                return 0;
            }
            if (LOWORD(wp) == kOpenFolder && g_filesReceived > 0) {
                ShellExecuteW(h, L"open", g_receiveFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        break;
    case WM_DRAWITEM:
        if (g_opt.pcPreview && (wp == kPcStatus || wp == kPcDisconnect ||
            (wp >= kPcFirstPeer && wp < kPcFirstPeer + docky::pairing::kPcPeerCount))) {
            PaintPhoneItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
        }
        if (wp == kClose || wp == kBack || wp == kConfirmText || wp == kConfirmStay || wp == kConfirmLeave ||
            wp == kQrToggle || wp == kStatus || wp == kGuide || wp == kBallSize ||
            wp == kReceiveSettings || wp == kCopyText || wp == kOpenFolder || wp == kSendFiles || wp == kSendText) {
            PaintPhoneItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
        }
        break;
    case WM_SIZE:
        if (g_opt.pcPreview && g_pcStatus) LayoutPcControls(h);
        if (g_opt.pairPhone && g_status) LayoutPhoneControls(h);
        break;
#endif
    case WM_NCHITTEST:
#ifdef DOCKY_PHONE_PAIRING
        if (IsPcReceiveBall()) return HTCLIENT;  // the native ellipse region rejects the transparent corners
        if (g_opt.pcPreview) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &point);
            if (PcPeerAt(point, Now()) >= 0) return HTCLIENT;
        }
        if (g_opt.pairPhone && !g_showQr) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            float normal[3]{};
            if (PickBallSurface(p, normal)) return HTCLIENT;
        }
#endif
        return HTCAPTION;  // the area around the glass moves the window
#ifdef DOCKY_PHONE_PAIRING
    case WM_SETCURSOR:
        if (IsPcReceiveBall() && LOWORD(lp) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(h, &point);
            SetCursor(LoadCursorW(nullptr, PcReceiveActionAt(h, point) == PcReceiveAction::None ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (IsPcReceiveBall()) {
            if (g_pcReceivePressed != PcReceiveAction::None && PcReceiveDragExceeded(h, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
                g_pcReceivePressed = PcReceiveAction::None;
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (IsPcReceiveBall()) { g_pcReceivePressed = PcReceiveAction::None; return 0; }
        break;
    case WM_LBUTTONDOWN:
        if (IsPcReceiveBall()) {
            g_pcReceivePressPoint = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g_pcReceivePressed = PcReceiveActionAt(h, g_pcReceivePressPoint);
            if (g_pcReceivePressed != PcReceiveAction::None) SetCapture(h);
            return 0;
        }
        if (g_opt.pcPreview) {
            const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int peer = PcPeerAt(point, Now());
            if (peer >= 0) {
                SetFocus(g_pcPeers[peer]);
                RequestDemoConnection(peer, Now());
            }
            return 0;
        }
        if (g_opt.pairPhone && !g_showQr && !g_reduceMotion) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            float normal[3]{};
            if (PickBallSurface(p, normal)) TapBall(normal, Now());
        }
        return 0;
    case WM_LBUTTONUP:
        if (IsPcReceiveBall()) {
            const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const auto pressed = g_pcReceivePressed;
            const bool invoke = GetCapture() == h && pressed != PcReceiveAction::None &&
                !PcReceiveDragExceeded(h, point) && pressed == PcReceiveActionAt(h, point);
            g_pcReceivePressed = PcReceiveAction::None;
            if (GetCapture() == h) ReleaseCapture();
            if (invoke && pressed == PcReceiveAction::Close) ClosePcBall();
            else if (invoke) ConfirmPcBall();
            return 0;
        }
        break;
#endif
    case WM_KEYDOWN:
#ifdef DOCKY_PHONE_PAIRING
        if (g_pcOffer) {
            if (wp == VK_ESCAPE) ClosePcBall();
            else if (IsPcReceiveBall() && wp == VK_RETURN && !(lp & 0x40000000)) ConfirmPcBall();
            return 0;
        }
#endif
        if (wp == VK_ESCAPE) g_quit = true;
        return 0;
    case WM_ENTERSIZEMOVE: SetTimer(h, 1, 8, nullptr); return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(h, 1);
#ifdef DOCKY_PHONE_PAIRING
        if (g_opt.pairPhone) {
            RECT window{};
            if (GetWindowRect(h, &window)) FitPhoneWindow(h, window, GetDpiForWindow(h));
        }
#endif
        return 0;
    case WM_TIMER:
#ifdef DOCKY_PHONE_PAIRING
        if (wp == kSaveRadiusTimer) { SaveRadius(); return 0; }
#endif
        if (g_app) g_app->Frame();
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
#ifdef DOCKY_PHONE_PAIRING
        if (IsPcReceiveBall()) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const auto target = PcReceivePlacement(monitor.rcWork, HIWORD(wp));
                const int size = target.right - target.left;
                SetWindowPos(h, nullptr, target.left, target.top, size, size, SWP_NOZORDER | SWP_NOACTIVATE);
                SetPcReceiveRegion(h, size);
                return 0;
            }
        }
        if (g_opt.pairPhone) {
            FitPhoneWindow(h, *r, HIWORD(wp));
            return 0;
        }
#endif
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
#ifdef DOCKY_PHONE_PAIRING
        if (g_pcOffer) { ClosePcBall(); return 0; }
        // Taskbar close and Alt+F4 also ask first while a transfer is running.
        if (g_opt.pairPhone && g_transferring && g_finishedAt < 0 && g_confirmText) {
            ShowBackConfirm(true, true);
            return 0;
        }
#endif
        g_quit = true; return 0;
    case WM_DESTROY:
#ifdef DOCKY_PHONE_PAIRING
        if (g_pcOffer) g_quit = true;
#endif
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

#ifdef DOCKY_PHONE_PAIRING
int RunPcBallSession(HINSTANCE instance, docky::pairing::pc_receive::Offer& offer) {
    g_pcOffer = &offer;
    g_opt.pairPhone = false;
    g_opt.pcPreview = offer.sending;
    g_opt.mode = 3;
    BOOL animation = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    g_reduceMotion = !animation;
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = offer.sending ? kPcSendBallWindow : kPcReceiveBallWindow;
    RegisterClassW(&wc);
    const HWND foreground = offer.sending ? nullptr : GetForegroundWindow();
    auto monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) {
        monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &mi) &&
            !SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0)) {
            mi.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
    }
    UINT dpi = 96, vertical = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical))) dpi = 96;
    const auto rc = mi.rcWork;
    const auto receive = PcReceivePlacement(rc, dpi);
    const int width = offer.sending ? MulDiv(486, dpi, 96) : receive.right - receive.left;
    const int height = offer.sending ? MulDiv(530, dpi, 96) : receive.bottom - receive.top;
    const int x = offer.sending ? (rc.left + rc.right - width) / 2 : receive.left;
    const int y = offer.sending ? (rc.top + rc.bottom - height) / 2 : receive.top;
    const DWORD style = WS_EX_NOREDIRECTIONBITMAP | (offer.sending ? 0 : WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    g_wnd = CreateWindowExW(style, wc.lpszClassName, L"DockDrop", WS_POPUP, x, y, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!g_wnd) { g_pcOffer = nullptr; return 1; }
    if (!offer.sending) {
        SetPcReceiveRegion(g_wnd, width);
    } else {
        const double now = Now();
        g_pcMotion.Discover(kLivePeer, now - 1);
        g_pcMotion.Request(kLivePeer, now);
    }
    App app;
    g_app = &app;
    if (!app.Init()) {
        DestroyWindow(g_wnd);
        g_app = nullptr;
        g_pcOffer = nullptr;
        return 1;
    }
    while (!g_quit) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_quit) {
            app.Frame();
            if (!g_quit && app.Presented() && !IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
            docky::vblank::WaitFrame();
        }
    }
    app.Shutdown();
    g_app = nullptr;
    if (IsWindow(g_wnd)) DestroyWindow(g_wnd);
    g_pcOffer = nullptr;
    return 0;
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#ifdef DOCKY_PHONE_PAIRING
    if (argc == 6 && (std::wstring_view(argv[1]) == L"--pc-receive-offer" ||
                     std::wstring_view(argv[1]) == L"--pc-send-progress")) {
        docky::pairing::pc_receive::Offer offer;
        offer.sending = std::wstring_view(argv[1]) == L"--pc-send-progress";
        offer.sender = argv[2]; offer.file = argv[3]; offer.bytes = argv[4]; offer.folder = argv[5];
        LocalFree(argv);
        std::wstring settingsPath;
#ifdef DOCKDROP_STANDALONE
        wchar_t local[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
        if (size && size < ARRAYSIZE(local)) settingsPath = std::wstring(local, size) + L"\\DockDrop\\settings.json";
#else
        settingsPath = liquid::settings::DefaultPath();
#endif
        offer.bubble = liquid::settings::Load(settingsPath).value.pcReceiveBubble;
        const int status = offer.bubble || offer.sending ? RunPcBallSession(inst, offer) :
            docky::pairing::pc_receive::RunBanner(inst, offer);
        CoUninitialize();
        return status;
    }
#endif
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--studio") g_opt.mode = 2;
#ifdef DOCKY_PHONE_PAIRING
        else if (a == L"--demo-pc") g_opt.pcPreview = true;
        else if (a == L"--demo-pc-tour") { g_opt.pcPreview = true; g_opt.pcTour = true; }
        else if (a == L"--reduce-motion") g_opt.pcReduced = true;
        else if (a == L"--pair-phone") g_opt.pairPhone = true;
        else if (a == L"--send-file") {
            g_opt.pairPhone = true;
            g_shellSendPath = next();
        }
        else if (a == L"--register-dockdrop-menu") g_opt.registerDockDrop = true;
        else if (a == L"--settings-path") g_opt.settingsPath = next();
#endif
        else if (a == L"--studio-dark") g_opt.mode = 3;
        else if (a == L"--image") { g_opt.mode = 1; g_opt.image = next(); }
        else if (a == L"--scroll") g_opt.scroll = _wtof(next().c_str());
        else if (a == L"--radius") g_opt.radius = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--size") { g_opt.sizeW = static_cast<float>(_wtof(next().c_str())); g_opt.sizeH = static_cast<float>(_wtof(next().c_str())); }
        else if (a == L"--no-bubbles") g_opt.bubbles = false;
        else if (a == L"--code-a") g_opt.codeMode = 1;
        else if (a == L"--code-rays") { g_opt.codeMode = 4; g_opt.codeAmp = static_cast<float>(_wtof(next().c_str())); }
        else if (a == L"--code-b") { g_opt.codeMode = 2; g_opt.codeAmp = static_cast<float>(_wtof(next().c_str())); }
        else if (a == L"--cells") { const std::wstring b = next(); for (size_t k = 0; k < b.size() && k < 256; ++k) if (b[k] == L'1') g_opt.codeBits[k >> 5] |= 1u << (k & 31); }
        else if (a == L"--ripple") g_opt.ripple = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--snapshot-seq") g_opt.seqDir = next();
        else if (a == L"--snapshot-after") g_opt.seqAfter = _wtof(next().c_str());
        else if (a == L"--snapshot-interval") g_opt.seqInterval = std::max(0.005, _wtof(next().c_str()));
        else if (a == L"--snapshot-count") g_opt.seqCount = _wtoi(next().c_str());
        else if (a == L"--exit-after") g_opt.exitAfter = _wtof(next().c_str());
        else if (a == L"--demo-progress") g_opt.demoProgress = std::max(0.0, _wtof(next().c_str()));
        else if (a == L"--demo-session") { g_opt.demoSession = next(); std::replace(g_opt.demoSession.begin(), g_opt.demoSession.end(), L'|', L'\n'); }
        else if (a == L"--bench-out") g_opt.benchOut = next();
    }
    LocalFree(argv);

#ifdef DOCKY_PHONE_PAIRING
    const auto languagePath = g_opt.settingsPath.empty() ? liquid::settings::DefaultPath() : g_opt.settingsPath;
    if (liquid::settings::Load(languagePath).value.appearance.language == "zh-CN")
        docky::pairing::uiLanguage = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    if (g_opt.registerDockDrop) {
        wchar_t path[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        return len && len < ARRAYSIZE(path) && docky::pairing::RegisterDockDropMenu(path) ? 0 : 1;
    }
    if (g_opt.pcPreview) {
        g_opt.pairPhone = false;
        g_opt.mode = 3;
        g_opt.codeMode = 0;
        if (g_opt.sizeW <= 0) g_opt.sizeW = 610;
        if (g_opt.sizeH <= 0) g_opt.sizeH = 680;
    }
    HANDLE singleInstance = nullptr;
    if (g_opt.pairPhone) {
        singleInstance = CreateMutexW(nullptr, FALSE, kPhoneMutex);
        if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND window = FindWindowW(kPhoneWindow, nullptr)) {
                ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
                if (!g_shellSendPath.empty()) {
                    COPYDATASTRUCT message{kShellSendFile,
                        static_cast<DWORD>((g_shellSendPath.size() + 1) * sizeof(wchar_t)), g_shellSendPath.data()};
                    DWORD_PTR queued = 0;
                    if (!SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&message),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &queued) || !queued)
                        MessageBoxW(nullptr, Ui(L"请先完成当前的 DockDrop 操作，再发送这个文件。"),
                            L"DockDrop", MB_OK | MB_ICONINFORMATION);
                } else {
                    COPYDATASTRUCT message{docky::pairing::kLoadWidgetDraft, 0, nullptr};
                    DWORD_PTR queued = 0;
                    SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&message),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &queued);
                }
            }
            CloseHandle(singleInstance);
            return 0;
        }
        g_opt.mode = 3;
        g_opt.codeMode = 0;
        g_opt.codeAmp = 0.07f;
        wchar_t localAppData[MAX_PATH]{};
        const DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
#ifdef DOCKDROP_STANDALONE
        if (g_opt.settingsPath.empty() && localLength && localLength < ARRAYSIZE(localAppData))
            g_opt.settingsPath = std::wstring(localAppData) + L"\\DockDrop\\settings.json";
#else
        if (localLength && localLength < ARRAYSIZE(localAppData)) {
            dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(localAppData, localLength) + L"\\LiquidDock\\glass.json");
        }
        if (g_opt.settingsPath.empty()) g_opt.settingsPath = liquid::settings::DefaultPath();
#endif
        const auto settings = liquid::settings::Load(g_opt.settingsPath).value;
        g_preferredRadius = settings.phoneBallRadius;
        g_receiveFolder = settings.phoneReceiveFolder;
        if (g_receiveFolder.empty()) g_receiveFolder = docky::pairing::DownloadFolder();
        g_opt.radius = static_cast<float>(g_preferredRadius);
        g_opt.sizeW = 610;
        g_opt.sizeH = 780;
    }
#endif
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"DockyBubble";
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
#ifdef DOCKY_PHONE_PAIRING
    if (g_opt.pairPhone) wc.lpszClassName = kPhoneWindow;
    if (g_opt.pcPreview) wc.lpszClassName = kPcWindow;
#endif
    RegisterClassExW(&wc);
    POINT cursor; GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    const float dpi = dx / 96.f;
    const float sw = g_opt.sizeW > 0 ? g_opt.sizeW : g_opt.radius * 3.2f, sh = g_opt.sizeH > 0 ? g_opt.sizeH : g_opt.radius * 3.2f;
    int w = static_cast<int>(sw * dpi), h = static_cast<int>(sh * dpi);
#ifdef DOCKY_PHONE_PAIRING
    if (g_opt.pairPhone || g_opt.pcPreview) {
        const auto size = PhoneWindowSize(mi.rcWork, dpi);
        w = size.cx;
        h = size.cy;
    }
#endif
    const int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    const bool panel = g_opt.pairPhone || g_opt.pcPreview;
    const DWORD exStyle = g_opt.pcPreview ? WS_EX_CONTROLPARENT :
        g_opt.pairPhone ? 0 : WS_EX_NOREDIRECTIONBITMAP;
    const DWORD style = WS_POPUP | (panel ? WS_CLIPCHILDREN : 0);
    const wchar_t* title = g_opt.pcPreview ? Ui(L"DockDrop · 电脑互传预览")
        : g_opt.pairPhone ? Ui(L"DockDrop · 配对手机") : L"Optical Bubble";
    g_wnd = CreateWindowExW(exStyle, wc.lpszClassName, title,
        style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    if (!panel) SetWindowDisplayAffinity(g_wnd, WDA_EXCLUDEFROMCAPTURE);
#ifdef DOCKY_PHONE_PAIRING
    PhonePaintRuntime phonePaint;
    if (panel) {
        phonePaint.Init();
        const MARGINS fullWindow{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(g_wnd, &fullWindow);
        WNDCLASSW surface{};
        surface.hInstance = inst;
        surface.lpfnWndProc = SurfaceProc;
        surface.lpszClassName = L"DockyPhonePairingSurface";
        RegisterClassW(&surface);
        g_surface = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, surface.lpszClassName, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w, h, g_wnd, nullptr, inst, nullptr);
        if (!g_surface) return 1;
    }
#endif
    App app; g_app = &app;
    if (!app.Init()) { MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"Optical Bubble", MB_ICONERROR); return 1; }
    ShowWindow(g_wnd, SW_SHOW);
#ifdef DOCKY_PHONE_PAIRING
    if (g_opt.pcPreview) CreatePcControls(g_wnd);
    if (g_opt.pairPhone) {
        CreatePhoneControls(g_wnd);
        DragAcceptFiles(g_wnd, TRUE);
        if (g_opt.demoSession.empty()) {
            if (g_shellSendPath.empty()) LoadWidgetDraft();
            g_phoneHost = docky::pairing::StartPhoneHost(g_wnd, ReceivePhoneMessage);
            if (!g_shellSendPath.empty()) PostMessageW(g_wnd, kShellSendReady, 0, 0);
        } else {
            g_opt.codeMode = 4;
            ReceivePhoneMessage(L"paired");
            ReceivePhoneMessage(L"receive-text:" + g_opt.demoSession);
            ReceivePhoneMessage(L"transfer-progress:4000");
        }
    }
#endif
    while (!g_quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
#ifdef DOCKY_PHONE_PAIRING
            if (g_opt.pairPhone && g_confirming && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
                ShowBackConfirm(false);
                continue;
            }
            if (g_opt.pairPhone && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && g_transferring && g_finishedAt < 0) {
                ShowBackConfirm(true, true);
                continue;
            }
#endif
            if (m.message == WM_QUIT || (panel && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE)) {
                g_quit = true;
                break;
            }
            if (panel && IsDialogMessageW(g_wnd, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (g_quit) break;
        app.Frame();
        docky::vblank::WaitFrame();
    }
    app.Shutdown();
#ifdef DOCKY_PHONE_PAIRING
    if (g_opt.pairPhone) SaveRadius();
    g_incoming.Cancel();
    g_phoneHost.reset();
    if (singleInstance) CloseHandle(singleInstance);
#endif
    g_app = nullptr;
    DestroyWindow(g_wnd);
#ifdef DOCKY_PHONE_PAIRING
    if (g_uiFont) DeleteObject(g_uiFont);
#endif
    return 0;
}
