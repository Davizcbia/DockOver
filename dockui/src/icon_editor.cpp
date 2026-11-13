#include "icon_editor.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "msimg32.lib")  // AlphaBlend for the preview

namespace dock::iconedit {
namespace {

constexpr wchar_t kClass[] = L"LiquidDockIconEditor";
enum : int { kScale = 101, kOffX, kOffY, kMask, kShadow, kChange, kReset, kApply, kCancel, kScaleText, kOffXText, kOffYText };

struct Editor {
    HWND hwnd = nullptr, owner = nullptr;
    State state;
    Callbacks cb;
    UINT dpi = 96;
    HFONT font = nullptr;
    RECT preview{};
    std::vector<uint32_t> rendered;
    UINT side = 0;
};
Editor* g_editor = nullptr;

int Px(const Editor* e, int v) { return MulDiv(v, static_cast<int>(e->dpi), 96); }

HWND Make(Editor* e, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Px(e, x), Px(e, y), Px(e, w), Px(e, h), e->hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (c && e->font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(e->font), TRUE);
    return c;
}

void SetCheck3(HWND c, int v) { SendMessageW(c, BM_SETCHECK, v < 0 ? BST_INDETERMINATE : v ? BST_CHECKED : BST_UNCHECKED, 0); }
int GetCheck3(HWND c) { const LRESULT s = SendMessageW(c, BM_GETCHECK, 0, 0); return s == BST_INDETERMINATE ? -1 : s == BST_CHECKED ? 1 : 0; }

void SyncControls(Editor* e) {
    const IconEdit& d = e->state.edit;
    SendDlgItemMessageW(e->hwnd, kScale, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(d.scale * 100)));
    SendDlgItemMessageW(e->hwnd, kOffX, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(d.offsetX * 100)));
    SendDlgItemMessageW(e->hwnd, kOffY, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(d.offsetY * 100)));
    SetCheck3(GetDlgItem(e->hwnd, kMask), d.mask);
    SetCheck3(GetDlgItem(e->hwnd, kShadow), d.shadow);
    wchar_t t[48];
    swprintf_s(t, L"%d%%", static_cast<int>(std::lround(d.scale * 100)));
    SetDlgItemTextW(e->hwnd, kScaleText, t);
    swprintf_s(t, L"%+d%%", static_cast<int>(std::lround(d.offsetX * 100)));
    SetDlgItemTextW(e->hwnd, kOffXText, t);
    swprintf_s(t, L"%+d%%", static_cast<int>(std::lround(d.offsetY * 100)));
    SetDlgItemTextW(e->hwnd, kOffYText, t);
}

void Refresh(Editor* e) {
    if (e->cb.render) e->rendered = e->cb.render(e->state, e->side);
    InvalidateRect(e->hwnd, &e->preview, FALSE);
}

void ReadControls(Editor* e) {
    IconEdit& d = e->state.edit;
    d.scale = SendDlgItemMessageW(e->hwnd, kScale, TBM_GETPOS, 0, 0) / 100.0;
    d.offsetX = static_cast<int>(SendDlgItemMessageW(e->hwnd, kOffX, TBM_GETPOS, 0, 0)) / 100.0;
    d.offsetY = static_cast<int>(SendDlgItemMessageW(e->hwnd, kOffY, TBM_GETPOS, 0, 0)) / 100.0;
    d.mask = GetCheck3(GetDlgItem(e->hwnd, kMask));
    d.shadow = GetCheck3(GetDlgItem(e->hwnd, kShadow));
    d.Clamp();
}

void PaintPreview(Editor* e, HDC dc) {
    // Neutral two-tone backdrop (light top, dark bottom) so mask edges and transparency read on both.
    RECT r = e->preview;
    RECT top{r.left, r.top, r.right, (r.top + r.bottom) / 2}, bottom{r.left, top.bottom, r.right, r.bottom};
    HBRUSH light = CreateSolidBrush(RGB(232, 234, 238)), darkB = CreateSolidBrush(RGB(52, 54, 60));
    FillRect(dc, &top, light);
    FillRect(dc, &bottom, darkB);
    DeleteObject(light);
    DeleteObject(darkB);
    if (e->rendered.empty() || !e->side) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(e->side);
    bi.bmiHeader.biHeight = -static_cast<LONG>(e->side);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        // AlphaBlend needs premultiplied pixels; premultiply defensively (straight-alpha sources).
        auto* px = static_cast<uint32_t*>(bits);
        for (size_t i = 0; i < e->rendered.size() && i < static_cast<size_t>(e->side) * e->side; ++i) {
            uint32_t c = e->rendered[i];
            const uint32_t a = c >> 24;
            uint32_t b = c & 255, g = (c >> 8) & 255, rr = (c >> 16) & 255;
            if (b > a || g > a || rr > a) { b = b * a / 255; g = g * a / 255; rr = rr * a / 255; }
            px[i] = (a << 24) | (rr << 16) | (g << 8) | b;
        }
        HGDIOBJ old = SelectObject(mem, bmp);
        const int box = (std::min)(r.right - r.left, r.bottom - r.top) - Px(e, 24);
        const int x = (r.left + r.right - box) / 2, y = (r.top + r.bottom - box) / 2;
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        SetStretchBltMode(dc, HALFTONE);
        AlphaBlend(dc, x, y, box, box, mem, 0, 0, static_cast<int>(e->side), static_cast<int>(e->side), bf);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Editor* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE: {
        e = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        e->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
        break;
    }
    case WM_CREATE: {
        e->dpi = GetDpiForWindow(h) ? GetDpiForWindow(h) : 96;
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, e->dpi);
        e->font = CreateFontIndirectW(&ncm.lfMessageFont);
        e->preview = {Px(e, 16), Px(e, 16), Px(e, 176), Px(e, 176)};
        auto label = [&](const wchar_t* t, int y) { Make(e, WC_STATICW, t, SS_LEFT, -1, 196, y, 120, 20); };
        label(L"Scale", 18);
        Make(e, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kScale, 192, 38, 200, 28);
        Make(e, WC_STATICW, L"", SS_RIGHT, kScaleText, 340, 18, 52, 20);
        label(L"Horizontal offset", 72);
        Make(e, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kOffX, 192, 92, 200, 28);
        Make(e, WC_STATICW, L"", SS_RIGHT, kOffXText, 340, 72, 52, 20);
        label(L"Vertical offset", 126);
        Make(e, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kOffY, 192, 146, 200, 28);
        Make(e, WC_STATICW, L"", SS_RIGHT, kOffYText, 340, 126, 52, 20);
        SendDlgItemMessageW(h, kScale, TBM_SETRANGE, TRUE, MAKELPARAM(50, 150));
        SendDlgItemMessageW(h, kOffX, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<WORD>(-25), 25));
        SendDlgItemMessageW(h, kOffY, TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<WORD>(-25), 25));
        SendDlgItemMessageW(h, kOffX, TBM_SETRANGEMIN, TRUE, -25);
        SendDlgItemMessageW(h, kOffY, TBM_SETRANGEMIN, TRUE, -25);
        // Three-state: indeterminate = follow the Dock-wide setting.
        Make(e, WC_BUTTONW, L"macOS icon mask (grey = Dock default)", BS_AUTO3STATE | WS_TABSTOP, kMask, 16, 188, 380, 22);
        Make(e, WC_BUTTONW, L"Icon shadow (grey = Dock default)", BS_AUTO3STATE | WS_TABSTOP, kShadow, 16, 212, 380, 22);
        Make(e, WC_BUTTONW, L"Change Image…", BS_PUSHBUTTON | WS_TABSTOP, kChange, 16, 246, 120, 28);
        Make(e, WC_BUTTONW, L"Reset", BS_PUSHBUTTON | WS_TABSTOP, kReset, 144, 246, 80, 28);
        Make(e, WC_BUTTONW, L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, kApply, 232, 246, 80, 28);
        Make(e, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kCancel, 316, 246, 80, 28);
        SyncControls(e);
        Refresh(e);
        return 0;
    }
    case WM_HSCROLL:
        ReadControls(e);
        SyncControls(e);
        Refresh(e);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id == kMask || id == kShadow) { ReadControls(e); Refresh(e); }
        else if (id == kChange && e->cb.changeImage && e->cb.changeImage(e->state)) { e->state.resetAll = false; Refresh(e); }
        else if (id == kReset) {
            e->state.edit = IconEdit{};
            e->state.newImage.clear();
            e->state.resetAll = true;
            if (e->cb.resetToOriginal) e->cb.resetToOriginal(e->state);
            SyncControls(e);
            Refresh(e);
        } else if (id == kApply) {
            ReadControls(e);
            if (!e->cb.apply || e->cb.apply(e->state)) DestroyWindow(h);
        } else if (id == kCancel) DestroyWindow(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        PaintPreview(e, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);  // same as Cancel: nothing is persisted
        return 0;
    case WM_NCDESTROY: {
        if (e) {
            if (e->font) DeleteObject(e->font);
            if (e->owner && IsWindow(e->owner)) EnableWindow(e->owner, TRUE);
            if (g_editor == e) g_editor = nullptr;
            delete e;
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool Open(HWND owner, const std::wstring& title, State initial, Callbacks callbacks) {
    if (g_editor && IsWindow(g_editor->hwnd)) { SetForegroundWindow(g_editor->hwnd); return true; }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    auto* e = new Editor();
    e->owner = owner;
    e->state = std::move(initial);
    e->state.edit.Clamp();
    e->cb = std::move(callbacks);
    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    RECT want{0, 0, MulDiv(412, static_cast<int>(dpi ? dpi : 96), 96), MulDiv(290, static_cast<int>(dpi ? dpi : 96), 96)};
    AdjustWindowRectExForDpi(&want, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi ? dpi : 96);
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int W = want.right - want.left, H = want.bottom - want.top;
    const int x = (mi.rcWork.left + mi.rcWork.right - W) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - H) / 2;
    g_editor = e;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClass, (L"Edit Icon — " + title).c_str(), WS_CAPTION | WS_SYSMENU,
                             x, y, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), e);
    if (!h) { if (g_editor == e) { g_editor = nullptr; delete e; } return false; }
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
    return true;
}

bool IsOpen() { return g_editor && IsWindow(g_editor->hwnd); }
void Close() { if (IsOpen()) DestroyWindow(g_editor->hwnd); }

}  // namespace dock::iconedit
