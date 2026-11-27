#include "status_group.h"
#include "custom_icons.h"
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <string_view>
using Microsoft::WRL::ComPtr;

using namespace dock::statusgroup;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
using Cells = std::vector<std::wstring>;

// --render <out.png> <size> [wifi icon]: the combined cell alone on a grey plate (visual comparison with the reference).
static int Render(const wchar_t* out, int size, const wchar_t* icon, const State* given = nullptr) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic; ComPtr<ID2D1Factory1> d2d; ComPtr<IDWriteFactory> dw;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 10;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return 11;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    ComPtr<IWICBitmap> bmp; wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt))) return 12;
    ComPtr<ID2D1DeviceContext> dc; if (FAILED(rt.As(&dc))) return 13;
    State st; st.batteryPresent = false; st.wifi = 2; st.audio = true; st.volume = 40;
    if (given) st = *given;
    if (icon) { std::vector<uint32_t> px; UINT w = 0, h = 0; std::wstring err;
        if (dock::LoadCustomIconPixels(icon, px, w, h, err)) { st.wifiIcon = std::make_shared<const std::vector<uint32_t>>(std::move(px)); st.wifiW = w; st.wifiH = h; } }
    dc->BeginDraw(); dc->Clear(st.dark ? D2D1::ColorF(0.30f, 0.32f, 0.36f, 1.f) : D2D1::ColorF(0.93f, 0.93f, 0.94f, 1.f));
    Draw(dc.Get(), dw.Get(), D2D1::RectF(0, 0, static_cast<float>(size), static_cast<float>(size)), {L"combo"}, st, -1, 1.f);
    if (FAILED(dc->EndDraw())) return 14;
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    wic->CreateStream(&stream); if (FAILED(stream->InitializeFromFilename(out, GENERIC_WRITE))) return 15;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc); enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr); frame->Initialize(nullptr); frame->WriteSource(bmp.Get(), nullptr); frame->Commit(); enc->Commit();
    return 0;
}

static int RenderCell(const wchar_t* out, int size, const wchar_t* id, const State& st) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic; ComPtr<ID2D1Factory1> d2d; ComPtr<IDWriteFactory> dw;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 10;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return 11;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    const int pad = size / 20, full = size + 2 * pad;
    ComPtr<IWICBitmap> bmp; wic->CreateBitmap(full, full, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt))) return 12;
    ComPtr<ID2D1DeviceContext> dc; if (FAILED(rt.As(&dc))) return 13;
    dc->BeginDraw(); dc->Clear(st.tileStyle == 0 ? D2D1::ColorF(1, 1, 1, 1) : st.tileStyle == 1 ? D2D1::ColorF(0.55f, 0.58f, 0.62f, 1) : D2D1::ColorF(0.20f, 0.35f, 0.55f, 1));
    Draw(dc.Get(), dw.Get(), D2D1::RectF(static_cast<float>(pad), static_cast<float>(pad), static_cast<float>(pad + size), static_cast<float>(pad + size)), {id}, st, -1, 1.f);
    if (FAILED(dc->EndDraw())) return 14;
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    wic->CreateStream(&stream); if (FAILED(stream->InitializeFromFilename(out, GENERIC_WRITE))) return 15;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc); enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr); frame->Initialize(nullptr); frame->WriteSource(bmp.Get(), nullptr); frame->Commit(); enc->Commit();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--render") return Render(argv[2], _wtoi(argv[3]), argc >= 5 ? argv[4] : nullptr);
    if (argc >= 3 && std::wstring(argv[1]) == L"--render-tiles") {  // <dir>: Clock and Calendar at 10:10:30 on Wed 15, three styles
        const std::wstring dir = argv[2];
        // Sample tray icons for the Hidden Icons folder (32 px, premultiplied): white glyphs (rings) and coloured discs.
        std::vector<State::TrayImage> trays;
        for (int k = 0; k < 7; ++k) {
            auto px = std::make_shared<std::vector<uint32_t>>(32 * 32, 0u);
            const bool glyph = k % 2 == 0; const uint32_t colors[4] = {0xFF2D8CFFu, 0xFF34C759u, 0xFFFF9500u, 0xFFAF52DEu};
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
                const float d = std::hypot(x - 15.5f, y - 15.5f);
                if (glyph ? (d < 13.f && d > 8.f) : d < 13.f) (*px)[static_cast<size_t>(y) * 32 + x] = glyph ? 0xFFFFFFFFu : colors[k / 2 % 4];
            }
            State::TrayImage t; t.w = t.h = 32; t.pixels = px; t.name = L"Tray " + std::to_wstring(k); trays.push_back(t);
        }
        for (int style = 0; style < 3; ++style) for (const wchar_t* id : {L"analog", L"date", L"digital", L"tray", L"tray-empty"}) {
            State st; st.hour = 10; st.minute = 10; st.second = 30; st.day = 15; st.weekday = L"Wed"; st.time = L"10:10"; st.tileStyle = style; st.dark = style != 0;
            const std::wstring cell = std::wstring(id) == L"tray-empty" ? L"tray" : id;
            if (cell == L"tray" && std::wstring(id) == L"tray") st.trayPreview = trays;
            if (const int r = RenderCell((dir + L"\\" + id + L"-" + std::to_wstring(style) + L".png").c_str(), argc >= 4 ? _wtoi(argv[3]) : 902, cell.c_str(), st)) return r;
        }
        return 0;
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--render-states") {  // <dir> [wifi icon]: battery states of the combined cell
        const std::wstring dir = argv[2]; const wchar_t* icon = argc >= 4 ? argv[3] : nullptr;
        struct Case { const wchar_t* name; int battery; bool charging, plugged, percent; } cases[] = {
            {L"battery-72", 72, false, false, false}, {L"low-16-percent", 16, false, false, true}, {L"charging", 64, true, true, false},
            {L"charging-percent", 64, true, true, true}, {L"plugged-full-percent", 100, false, true, true},
            {L"saver-48", 48, false, false, true}, {L"light-charging-percent", 64, true, true, true}, {L"light-72", 72, false, false, false}};
        for (const auto& c : cases) { State st; st.saver = std::wstring(c.name).rfind(L"saver", 0) == 0; st.dark = std::wstring(c.name).rfind(L"light", 0) != 0; st.batteryPresent = true; st.battery = c.battery; st.charging = c.charging; st.plugged = c.plugged; st.showPercent = c.percent;
            st.wifi = 2; st.audio = true; st.volume = 40; if (const int r = Render((dir + L"\\" + c.name + L".png").c_str(), 294, icon, &st)) return r; }
        return 0;
    }
    // Ordinary cells: two per column, a column is half an icon wide.
    CHECK(WidthFactor(Cells{}) == 0.f);
    CHECK(WidthFactor(Cells{L"wifi"}) == 0.5f && WidthFactor(Cells{L"wifi", L"sound"}) == 0.5f);
    CHECK(WidthFactor(Cells{L"wifi", L"sound", L"clock"}) == 1.f && WidthFactor(Cells{L"wifi", L"sound", L"clock", L"input"}) == 1.f);
    CHECK(WidthFactor(Cells{L"a", L"b", L"c", L"d", L"e"}) == 1.5f);
    // The combined status cell is a whole icon.
    CHECK(WidthFactor(Cells{L"combo"}) == 1.f);
    CHECK(WidthFactor({L"system"}) == 3.f && StandaloneCell(L"system"));
    const D2D1_RECT_F system{0,0,150,50};
    CHECK(std::wstring_view(SystemStatusPart(system,{10,25})) == L"wifi");
    CHECK(std::wstring_view(SystemStatusPart(system,{33,25})) == L"sound");
    CHECK(std::wstring_view(SystemStatusPart(system,{55,25})) == L"battery");
    CHECK(std::wstring_view(SystemStatusPart(system,{75,25})) == L"bluetooth");
    CHECK(std::wstring_view(SystemStatusPart(system,{120,25})) == L"digital");
    CHECK(WidthFactor(Cells{L"combo", L"control", L"clock"}) == 1.5f);
    CHECK(WidthFactor(Cells{L"combo", L"control", L"clock", L"input"}) == 2.f);
    CHECK(WidthFactor(Cells{L"analog"}) == 1.f && WidthFactor(Cells{L"date"}) == 1.f && StandaloneCell(L"analog") && StandaloneCell(L"date") && !StandaloneCell(L"clock"));
    CHECK(WidthFactor(Cells{L"control", L"combo"}) == 1.5f);  // a half-filled column before it stays half filled

    // Four ordinary cells in a full tile (46 x 46): column-major quarter squares.
    const D2D1_RECT_F body = D2D1::RectF(100, 10, 146, 56);
    const Cells four{L"wifi", L"sound", L"clock", L"input"};
    auto r0 = CellRect(body, four, 0), r1 = CellRect(body, four, 1), r2 = CellRect(body, four, 2), r3 = CellRect(body, four, 3);
    CHECK(Near(r0.left, 100) && Near(r0.top, 10) && Near(r0.right, 123) && Near(r0.bottom, 33));
    CHECK(Near(r1.left, 100) && Near(r1.top, 33));
    CHECK(Near(r2.left, 123) && Near(r2.top, 10));
    CHECK(Near(r3.left, 123) && Near(r3.bottom, 56));

    // Default group: combo (whole icon) then three half cells, in a body two icons wide (92 x 46).
    const D2D1_RECT_F wide = D2D1::RectF(0, 0, 92, 46);
    const Cells defaults{L"combo", L"control", L"clock", L"input"};
    auto c0 = CellRect(wide, defaults, 0), c1 = CellRect(wide, defaults, 1), c2 = CellRect(wide, defaults, 2), c3 = CellRect(wide, defaults, 3);
    CHECK(Near(c0.left, 0) && Near(c0.right, 46) && Near(c0.top, 0) && Near(c0.bottom, 46));
    CHECK(Near(c1.left, 46) && Near(c1.right, 69) && Near(c1.top, 0) && Near(c1.bottom, 23));
    CHECK(Near(c2.left, 46) && Near(c2.top, 23));
    CHECK(Near(c3.left, 69) && Near(c3.top, 0) && Near(c3.right, 92));

    // Hit testing follows the same cells.
    CHECK(CellAt(wide, defaults, D2D1::Point2F(20, 40)) == 0);
    CHECK(CellAt(wide, defaults, D2D1::Point2F(50, 5)) == 1);
    CHECK(CellAt(wide, defaults, D2D1::Point2F(50, 40)) == 2);
    CHECK(CellAt(wide, defaults, D2D1::Point2F(80, 5)) == 3);
    CHECK(CellAt(wide, defaults, D2D1::Point2F(80, 40)) == -1);  // empty slot
    CHECK(CellAt(body, four, D2D1::Point2F(99, 20)) == -1);

    CHECK(CellLabel(L"wifi") == L"Wi-Fi" && CellLabel(L"control") == L"Control Center" && CellLabel(L"ext:com.example.vpn") == L"com.example.vpn");
    // Tray icons kept in the Dock: half-size cells named after their exe; the plain "tray" cell is the chevron.
    CHECK(TrayCell(L"tray:clash-verge.exe#1") && !TrayCell(L"tray") && !TrayCell(L"tray:") && !TrayCell(L"ext:x"));
    CHECK(CellLabel(L"tray:clash-verge.exe#1") == L"clash-verge" && CellLabel(L"tray:Sync.EXE#{A}") == L"Sync" && CellLabel(L"tray") == L"Hidden Icons");
    CHECK(!FullCell(L"tray:clash-verge.exe#1"));
    CHECK(WidthFactor({L"tray:a.exe#1", L"tray:b.exe#2"}) == 0.5f && WidthFactor({L"tray:a.exe#1", L"tray:b.exe#2", L"tray:c.exe#3"}) == 1.0f);
    // Hidden Icons (folder) and Date & Time are whole-icon tiles of their own, on the Clock / Calendar plate.
    CHECK(FullCell(L"tray") && StandaloneCell(L"tray") && FullCell(L"digital") && StandaloneCell(L"digital") && WidthFactor(Cells{L"digital"}) == 1.f);
    CHECK(PlateCell(L"analog") && PlateCell(L"date") && PlateCell(L"digital") && PlateCell(L"tray") && !PlateCell(L"combo") && !PlateCell(L"clock"));
    CHECK(CellLabel(L"digital") == L"Date & Time");

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("status group: all checks passed\n");
    return 0;
}
