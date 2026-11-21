#include "stacks.h"
#include "glass_window.h"
#include "../vblank.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <psapi.h>
#include <sddl.h>
#include <thread>

namespace docky::desk {

// ---- Grouping ----------------------------------------------------------------------------------------------
std::wstring KindOf(const std::wstring& name, bool dir) {
    if (dir) return L"Folders";
    std::wstring lower = name; for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    const auto dot = lower.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : lower.substr(dot + 1);
    auto any = [&](std::initializer_list<const wchar_t*> list) { for (auto e : list) if (ext == e) return true; return false; };
    if (any({L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"heic", L"heif", L"tif", L"tiff", L"svg", L"ico", L"avif", L"raw"})) {
        if (lower.rfind(L"screenshot", 0) == 0 || lower.rfind(L"screen shot", 0) == 0 || lower.find(L"屏幕截图") != std::wstring::npos || lower.find(L"截图") != std::wstring::npos) return L"Screenshots";
        return L"Images";
    }
    if (any({L"mp4", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"m4v", L"flv"})) return L"Movies";
    if (any({L"mp3", L"wav", L"flac", L"aac", L"m4a", L"ogg", L"wma", L"opus"})) return L"Music";
    if (ext == L"pdf") return L"PDF Documents";
    if (any({L"xls", L"xlsx", L"csv", L"numbers", L"ods"})) return L"Spreadsheets";
    if (any({L"ppt", L"pptx", L"key", L"odp"})) return L"Presentations";
    if (any({L"doc", L"docx", L"txt", L"rtf", L"md", L"odt", L"pages", L"log", L"json", L"xml", L"html", L"htm"})) return L"Documents";
    if (any({L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"iso"})) return L"Archives";
    if (any({L"lnk", L"url", L"exe", L"msi", L"appref-ms", L"bat", L"cmd", L"ps1"})) return L"Apps & Shortcuts";
    return L"Other";
}

std::vector<Stack> BuildStacks(std::vector<FileEntry> files, GroupBy by, ULONGLONG now) {
    // Newest first inside every stack (the top of the pile is the most recent file).
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.modified > b.modified; });
    std::vector<Stack> out;
    auto add = [&](const std::wstring& group, const FileEntry& f) {
        auto it = std::find_if(out.begin(), out.end(), [&](const Stack& s) { return s.name == group; });
        if (it == out.end()) { out.push_back({group, {}}); it = out.end() - 1; }
        it->files.push_back(f);
    };
    if (by == GroupBy::Kind) {
        static const wchar_t* order[] = {L"Folders", L"Images", L"Screenshots", L"Movies", L"Music", L"PDF Documents", L"Documents",
                                         L"Spreadsheets", L"Presentations", L"Archives", L"Apps & Shortcuts", L"Other"};
        for (const auto* g : order) for (const auto& f : files) if (KindOf(f.name, f.dir) == g) add(g, f);
        return out;
    }
    const ULONGLONG day = 864000000000ull;
    const ULONGLONG today = now - now % day;
    static const wchar_t* names[] = {L"Today", L"Yesterday", L"Previous 7 Days", L"Previous 30 Days", L"Earlier"};
    for (int g = 0; g < 5; ++g)
        for (const auto& f : files) {
            const int group = f.modified >= today ? 0 : f.modified >= today - day ? 1 : f.modified >= today - 7 * day ? 2 : f.modified >= today - 30 * day ? 3 : 4;
            if (group == g) add(names[g], f);
        }
    return out;
}

std::wstring& DevFolder() { static std::wstring folder; return folder; }
std::vector<std::wstring> DesktopFolders() {
    if (!DevFolder().empty()) return {DevFolder()};
    std::vector<std::wstring> out;
    for (const auto& id : {FOLDERID_Desktop, FOLDERID_PublicDesktop}) {
        PWSTR folder = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder)) && folder) out.push_back(folder);
        if (folder) CoTaskMemFree(folder);
    }
    return out;
}
std::vector<FileEntry> ScanDesktop() {
    std::vector<FileEntry> out;
    for (const auto& base : DesktopFolders()) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (_wcsicmp(name.c_str(), L"desktop.ini") == 0 || name.rfind(L"~$", 0) == 0) continue;
            FileEntry f; f.path = base + L"\\" + name; f.name = name; f.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            FILETIME local{}; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local);
            f.modified = (static_cast<ULONGLONG>(local.dwHighDateTime) << 32) | local.dwLowDateTime;
            if (out.size() < 2000) out.push_back(std::move(f));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return out;
}

// ---- Explorer context menu -------------------------------------------------------------------------------------
namespace {
IContextMenu2* g_menu2 = nullptr; IContextMenu3* g_menu3 = nullptr;
}
bool ForwardShellMenuMessage(UINT msg, WPARAM w, LPARAM l, LRESULT& result) {
    if (msg != WM_INITMENUPOPUP && msg != WM_DRAWITEM && msg != WM_MEASUREITEM && msg != WM_MENUCHAR) return false;
    if (g_menu3) return SUCCEEDED(g_menu3->HandleMenuMsg2(msg, w, l, &result));
    if (g_menu2) { result = 0; return SUCCEEDED(g_menu2->HandleMenuMsg(msg, w, l)); }
    return false;
}
namespace {
// Several selected files as one shell item array (the user and public desktop are different folders; a UI object for
// items of different parents may fail, then callers fall back to the first item alone).
ComPtr<IShellItemArray> ItemArray(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& path : paths) { PIDLIST_ABSOLUTE id = nullptr; if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id) ids.push_back(id); }
    ComPtr<IShellItemArray> items;
    if (!ids.empty()) SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(reinterpret_cast<PIDLIST_ABSOLUTE*>(ids.data())), &items);
    for (auto id : ids) CoTaskMemFree(id);
    return items;
}
template <class T> bool BindMany(const std::vector<std::wstring>& paths, REFGUID bhid, ComPtr<T>& out) {
    if (paths.size() > 1) if (auto items = ItemArray(paths); items && SUCCEEDED(items->BindToHandler(nullptr, bhid, IID_PPV_ARGS(&out)))) return true;
    ComPtr<IShellItem> item;
    return !paths.empty() && SUCCEEDED(SHCreateItemFromParsingName(paths[0].c_str(), nullptr, IID_PPV_ARGS(&item))) && SUCCEEDED(item->BindToHandler(nullptr, bhid, IID_PPV_ARGS(&out)));
}
void RunMenu(HWND owner, IContextMenu* menu, POINT pt, UINT flags) {
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    if (SUCCEEDED(menu->QueryContextMenu(popup, 0, 1, 0x7FFF, flags))) {
        ComPtr<IContextMenu2> m2; ComPtr<IContextMenu3> m3;
        menu->QueryInterface(IID_PPV_ARGS(&m2)); menu->QueryInterface(IID_PPV_ARGS(&m3)); g_menu2 = m2.Get(); g_menu3 = m3.Get();
        SetForegroundWindow(owner);
        const UINT cmd = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
        g_menu2 = nullptr; g_menu3 = nullptr;
        if (cmd) {
            CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ci.hwnd = owner; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1); ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
            menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        }
        PostMessageW(owner, WM_NULL, 0, 0);
    }
    DestroyMenu(popup);
}
void ShellMenuMany(HWND owner, const std::vector<std::wstring>& paths, POINT pt) {
    ComPtr<IContextMenu> menu;
    if (BindMany(paths, BHID_SFUIObject, menu)) RunMenu(owner, menu.Get(), pt, CMF_NORMAL);
}
// The Explorer desktop background menu (View, Sort by, Refresh, Paste, New, Display settings, Personalize).
void DesktopBackgroundMenu(HWND owner, POINT pt) {
    ComPtr<IShellFolder> desktop; ComPtr<IContextMenu> menu;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && SUCCEEDED(desktop->CreateViewObject(owner, IID_PPV_ARGS(&menu)))) RunMenu(owner, menu.Get(), pt, CMF_NORMAL);
}
void DragOutMany(HWND window, const std::vector<std::wstring>& paths) {
    ComPtr<IDataObject> data;
    if (!BindMany(paths, BHID_DataObject, data)) return;
    DWORD effect = 0;
    SHDoDragDrop(window, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}
void ShellMenu(HWND owner, const std::wstring& path, POINT pt) {
    ComPtr<IShellItem> item; ComPtr<IContextMenu> menu;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu)))) return;
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    if (SUCCEEDED(menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL))) {
        ComPtr<IContextMenu2> m2; ComPtr<IContextMenu3> m3;
        menu.As(&m2); menu.As(&m3); g_menu2 = m2.Get(); g_menu3 = m3.Get();
        SetForegroundWindow(owner);
        const UINT cmd = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
        g_menu2 = nullptr; g_menu3 = nullptr;
        if (cmd) {
            CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ci.hwnd = owner; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1); ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
            menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        }
        PostMessageW(owner, WM_NULL, 0, 0);
    }
    DestroyMenu(popup);
}
void DragOut(HWND window, const std::wstring& path) {
    ComPtr<IShellItem> item; ComPtr<IDataObject> data;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return;
    DWORD effect = 0;
    SHDoDragDrop(window, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

// ---- Thumbnails (worker thread) --------------------------------------------------------------------------------
struct Image { std::vector<std::uint32_t> px; UINT w = 0, h = 0; bool ok = false; };
class Thumbs {
public:
    void Start(HWND notify) {
        notify_ = notify; stop_ = false;
        thread_ = std::thread([this] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (;;) {
                std::pair<std::wstring, int> job;
                {
                    std::unique_lock<std::mutex> l(m_);
                    cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
                    if (stop_) break;
                    job = queue_.front(); queue_.pop_front();
                }
                Image img;
                img.ok = ShellImage(job.first, job.second, true, img.px, img.w, img.h) || ShellImage(job.first, job.second, false, img.px, img.w, img.h);
                {
                    std::lock_guard<std::mutex> l(m_);
                    cache_[Key(job.first, job.second)] = std::move(img);
                    pending_.erase(Key(job.first, job.second));
                }
                if (notify_) PostMessageW(notify_, WM_APP + 7, 0, 0);
            }
            CoUninitialize();
        });
    }
    void Stop() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); if (thread_.joinable()) thread_.join(); }
    // Returns the image if ready, otherwise queues it (newest requests first).
    const Image* Get(const std::wstring& path, int px) {
        std::lock_guard<std::mutex> l(m_);
        const auto key = Key(path, px);
        if (auto it = cache_.find(key); it != cache_.end()) return &it->second;
        if (pending_.insert(key).second) queue_.push_front({path, px});
        cv_.notify_one();
        return nullptr;
    }
    void Clear() { std::lock_guard<std::mutex> l(m_); if (cache_.size() > 600) cache_.clear(); }
private:
    static std::wstring Key(const std::wstring& p, int px) { return p + L"|" + std::to_wstring(px); }
    std::mutex m_; std::condition_variable cv_; std::deque<std::pair<std::wstring, int>> queue_;
    std::map<std::wstring, Image> cache_; std::set<std::wstring> pending_;
    std::thread thread_; bool stop_ = false; HWND notify_ = nullptr;
};

// ---- Windows -------------------------------------------------------------------------------------------------
// Desktop grid: every tile (a stack, or a file of an expanded stack) is one desktop cell, filled top to bottom from
// the right edge of the work area, further columns to the left (macOS desktop order).
constexpr float kCellW = 96.f, kCellH = 104.f, kPile = 60.f, kIcon = 56.f;
constexpr float kTileW = 88.f, kTileH = 100.f, kTileIcon = 56.f;     // glass panel tiles
constexpr int kColumns = 4;
constexpr float kPanelPad = 14.f, kHeader = 44.f;
constexpr wchar_t kColumnClass[] = L"DockyDesktopStacks", kPanelClass[] = L"DockyDesktopStackPanel";
constexpr UINT kThumbReady = WM_APP + 7, kRescanMsg = WM_APP + 8;

struct Entry { int stack = -1, file = -1; D2D1_RECT_F rect{}; POINT cell{}; };  // file -1: the stack tile itself; cell: grid column from the right, row
// Expand / collapse animation (macOS): files fly out of the stack into their cells (and back in), every other tile
// glides to its new cell. Rectangles are absolute DIP (screen px / scale) so the window may change size meanwhile.
struct Flight { D2D1_RECT_F from{}, to{}; float fromAlpha = 1, toAlpha = 1; int entry = -1; FileEntry ghost; bool isGhost = false; };
constexpr double kFlightSeconds = 0.34;
struct State {
    HWND host = nullptr, column = nullptr;
    GlassWindow panel;
    GroupBy by = GroupBy::Kind; StackStyle style = StackStyle::InPlace;
    std::vector<Stack> stacks;
    std::vector<Entry> entries;               // DIP, column window coordinates
    std::set<std::wstring> expanded;          // in-place mode: stacks open on the desktop (by name, kept across rescans)
    std::set<std::wstring> sel; std::wstring anchor;  // selected file paths (desktop and panel); Shift-click anchor
    // Free placement (Settings > Stacks > Place stacks anywhere): every stack keeps its own desktop grid cell (x = column
    // from the right edge, y = row); expanded files flow from their stack's cell into the next free cells.
    bool freePlace = false; std::map<std::wstring, POINT> cells; std::function<void(const std::map<std::wstring, POINT>&)> cellsChanged;
    int gridCols = 1, gridRows = 1; RECT work{};
    bool draggingStack = false; std::wstring dragName;
    // Click catcher: an invisible window over the work area below every docky window, so a drag on the bare wallpaper
    // draws a selection rectangle (Explorer's own icon view is hidden while Stacks are on). Right-click shows Explorer's
    // desktop menu and files dropped there land on the desktop.
    HWND catcher = nullptr, band = nullptr; bool banding = false, bandShown = false; POINT bandStart{}; std::set<std::wstring> bandBase;
    int open = -1, hoverTile = -1, scroll = 0; // panel mode
    UINT dpi = 96; RECT columnRect{}, panelRect{};
    Canvas columnCanvas;
    Thumbs thumbs;
    std::thread watcher; HANDLE stopWatch = nullptr;
    bool raised = false, running = false;
    POINT press{}; bool pressed = false; int pressEntry = -1, pressTile = -1; bool mouseWasDown = false;
    std::vector<Flight> flights; double flightStart = 0; RECT flightWindow{};
    docky::vblank::Ticker flightTicker;  // expand / collapse flight: one step per display frame (was an 8 ms WM_TIMER = 64 Hz)
    docky::vblank::Ticker panelTicker;   // glass panel backdrop: polled every display frame while open
    std::map<const void*, ComPtr<ID2D1Bitmap>> bitmaps;  // thumbnail bitmaps of the column's render target
};
State& S() { static State s; return s; }
float K() { return S().dpi / 96.f; }
int Px(float dip) { return static_cast<int>(std::lround(dip * K())); }

State& S();
void PaintImage(ID2D1RenderTarget* rt, const Image* img, D2D1_RECT_F box, float opacity, float angleDeg, bool card, bool cache = false) {
    if (!img || !img->ok || img->px.empty()) return;
    ComPtr<ID2D1Bitmap> bmp;
    if (cache) { auto& b = S().bitmaps[img]; if (!b) b = MakeBitmap(rt, img->px.data(), img->w, img->h); bmp = b; }
    else bmp = MakeBitmap(rt, img->px.data(), img->w, img->h);
    if (!bmp) return;
    const float bw = static_cast<float>(img->w), bh = static_cast<float>(img->h), s = (std::min)((box.right - box.left) / bw, (box.bottom - box.top) / bh);
    const float w = bw * s, h = bh * s, cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    D2D1_MATRIX_3X2_F old; rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(cx, cy)) * old);
    const D2D1_RECT_F dst = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    if (card) {  // photos and documents read as small prints: white border and a hairline
        ComPtr<ID2D1SolidColorBrush> b; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, opacity), &b);
        const D2D1_RECT_F frame = D2D1::RectF(dst.left - 2, dst.top - 2, dst.right + 2, dst.bottom + 2);
        rt->FillRectangle(frame, b.Get()); b->SetColor(D2D1::ColorF(0, 0, 0, 0.18f * opacity)); rt->DrawRectangle(frame, b.Get(), 0.5f);
    }
    rt->DrawBitmap(bmp.Get(), dst, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt->SetTransform(old);
}
bool Opaque(const Image* img) {
    if (!img || !img->ok) return false;
    size_t clear = 0; for (auto p : img->px) if ((p >> 24) < 250) ++clear;
    return clear * 50 < img->px.size();
}
void LabelShadowed(ID2D1RenderTarget* rt, const std::wstring& text, IDWriteTextFormat* f, D2D1_RECT_F box) {
    // Desktop labels: white with a soft dark shadow so they read on any wallpaper (like Explorer's desktop icons).
    Text(rt, text, f, D2D1::RectF(box.left, box.top + 1, box.right, box.bottom + 1), D2D1::ColorF(0, 0, 0, 0.55f));
    Text(rt, text, f, box, D2D1::ColorF(1, 1, 1, 0.98f));
}
IDWriteTextFormat* TwoLine(float size) {
    static std::map<int, ComPtr<IDWriteTextFormat>> cache;
    auto& f = cache[static_cast<int>(size * 10)];
    if (!f && SUCCEEDED(DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(DWrite()->CreateEllipsisTrimmingSign(f.Get(), &sign))) { DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; f->SetTrimming(&t, sign.Get()); }
    }
    return f.Get();
}
std::wstring DisplayName(const FileEntry& f) {
    std::wstring name = f.name;
    if (!f.dir) { const auto dot = name.find_last_of(L'.'); if (dot != std::wstring::npos) { const auto ext = name.substr(dot); if (_wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".url") == 0) name.resize(dot); } }
    return name;
}
const FileEntry* EntryFile(int index) {
    auto& s = S();
    if (index < 0 || index >= static_cast<int>(s.entries.size())) return nullptr;
    const auto& e = s.entries[static_cast<size_t>(index)];
    if (e.file < 0 || e.stack < 0 || e.stack >= static_cast<int>(s.stacks.size())) return nullptr;
    const auto& files = s.stacks[static_cast<size_t>(e.stack)].files;
    return e.file < static_cast<int>(files.size()) ? &files[static_cast<size_t>(e.file)] : nullptr;
}

void LayoutColumn() {
    auto& s = S();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    s.dpi = GetDpiForSystem();
    if (s.column) { const UINT d = GetDpiForWindow(s.column); if (d) s.dpi = d; }
    const RECT work = mi.rcWork; s.work = work;
    const float workH = (work.bottom - work.top) / K() - 2 * kMargin, workW = (work.right - work.left) / K() - 2 * kMargin;
    const int perColumn = (std::max)(1, static_cast<int>(workH / kCellH)), maxColumns = (std::max)(1, static_cast<int>(workW / kCellW));
    s.gridCols = maxColumns; s.gridRows = perColumn;
    std::vector<Entry> flat;
    for (size_t i = 0; i < s.stacks.size(); ++i) {
        flat.push_back({static_cast<int>(i), -1, {}, {}});
        if (s.style == StackStyle::InPlace && s.expanded.count(s.stacks[i].name))
            for (size_t j = 0; j < s.stacks[i].files.size(); ++j) flat.push_back({static_cast<int>(i), static_cast<int>(j), {}, {}});
    }
    const int capacity = perColumn * maxColumns;
    if (!s.freePlace) {
        // Column order: top to bottom from the right edge, further columns to the left (macOS desktop order).
        if (flat.size() > static_cast<size_t>(capacity)) flat.resize(static_cast<size_t>(capacity));
        for (size_t i = 0; i < flat.size(); ++i) flat[i].cell = POINT{static_cast<LONG>(i) / perColumn, static_cast<LONG>(i) % perColumn};
    } else {
        std::vector<char> used(static_cast<size_t>(capacity), 0);
        auto index = [&](POINT c) { return static_cast<size_t>(c.x) * perColumn + c.y; };
        auto inGrid = [&](POINT c) { return c.x >= 0 && c.y >= 0 && c.x < maxColumns && c.y < perColumn; };
        auto cellAt = [&](size_t i) { return POINT{static_cast<LONG>(i / perColumn), static_cast<LONG>(i % perColumn)}; };
        std::vector<char> placed(flat.size(), 0);
        // 1. Stacks with a saved cell; 2. the others in column order; 3. expanded files after their stack.
        for (size_t i = 0; i < flat.size(); ++i) {
            if (flat[i].file >= 0) continue;
            auto it = s.cells.find(s.stacks[static_cast<size_t>(flat[i].stack)].name);
            if (it != s.cells.end() && inGrid(it->second) && !used[index(it->second)]) { flat[i].cell = it->second; used[index(it->second)] = 1; placed[i] = 1; }
        }
        for (size_t i = 0; i < flat.size(); ++i) {
            if (flat[i].file >= 0 || placed[i]) continue;
            for (size_t k = 0; k < used.size(); ++k) if (!used[k]) { flat[i].cell = cellAt(k); used[k] = 1; placed[i] = 1; break; }
        }
        POINT stackCell{};
        for (size_t i = 0; i < flat.size(); ++i) {
            if (flat[i].file < 0) { stackCell = flat[i].cell; continue; }
            if (!placed[i - flat[i].file - 1]) continue;  // its stack did not fit
            const size_t from = index(stackCell);
            for (size_t n = 1; n < used.size(); ++n) { const size_t k = (from + n) % used.size(); if (!used[k]) { flat[i].cell = cellAt(k); used[k] = 1; placed[i] = 1; break; } }
        }
        std::vector<Entry> kept;
        for (size_t i = 0; i < flat.size(); ++i) if (placed[i]) kept.push_back(flat[i]);
        flat = std::move(kept);
    }
    LONG minCol = 0, maxCol = 0, minRow = 0, maxRow = 0;
    for (size_t i = 0; i < flat.size(); ++i) {
        const POINT c = flat[i].cell;
        if (i == 0) { minCol = maxCol = c.x; minRow = maxRow = c.y; }
        minCol = (std::min)(minCol, c.x); maxCol = (std::max)(maxCol, c.x); minRow = (std::min)(minRow, c.y); maxRow = (std::max)(maxRow, c.y);
    }
    if (!s.freePlace) { minCol = 0; minRow = 0; }
    for (auto& e : flat) {
        const float x = (maxCol - e.cell.x) * kCellW, y = (e.cell.y - minRow) * kCellH;
        e.rect = D2D1::RectF(x, y, x + kCellW, y + kCellH);
    }
    const float w = (maxCol - minCol + 1) * kCellW, h = (maxRow - minRow + 1) * kCellH;
    s.entries = std::move(flat);
    const int right = work.right - Px(kMargin - 8) - Px(minCol * kCellW), top = work.top + Px(kMargin - 8) + Px(minRow * kCellH);
    s.columnRect = RECT{right - Px(w), top, right, top + (std::max)(1, Px(h))};
}
// The desktop grid cell under a screen point (free placement), clamped into the grid.
POINT CellAtScreen(POINT pt) {
    const auto& s = S();
    const float right = static_cast<float>(s.work.right - Px(kMargin - 8)), top = static_cast<float>(s.work.top + Px(kMargin - 8));
    const LONG col = static_cast<LONG>(std::floor((right - pt.x) / (kCellW * K()))), row = static_cast<LONG>(std::floor((pt.y - top) / (kCellH * K())));
    return POINT{std::clamp(col, 0L, static_cast<LONG>(s.gridCols - 1)), std::clamp(row, 0L, static_cast<LONG>(s.gridRows - 1))};
}
RECT EntryScreenRect(const Entry& e) {
    const auto& s = S();
    return RECT{s.columnRect.left + Px(e.rect.left), s.columnRect.top + Px(e.rect.top), s.columnRect.left + Px(e.rect.right), s.columnRect.top + Px(e.rect.bottom)};
}

double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return static_cast<double>(c.QuadPart) / f.QuadPart; }
D2D1_RECT_F Lerp(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float t) {
    return D2D1::RectF(a.left + (b.left - a.left) * t, a.top + (b.top - a.top) * t, a.right + (b.right - a.right) * t, a.bottom + (b.bottom - a.bottom) * t);
}
D2D1_RECT_F Absolute(const D2D1_RECT_F& r) { const auto& s = S(); return D2D1::RectF(r.left + s.columnRect.left / K(), r.top + s.columnRect.top / K(), r.right + s.columnRect.left / K(), r.bottom + s.columnRect.top / K()); }

// One tile. `c` is the cell in window DIP; `alpha` fades flying files.
void PaintTile(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const Stack* st, const FileEntry* file, D2D1_RECT_F c, float alpha, bool interactive) {
    auto& s = S();
    const int pilePx = Px(kPile * 1.25f), iconPx = Px(kIcon * 1.25f);
    if (interactive) { brush->SetColor(D2D1::ColorF(0, 0, 0, 1.f / 255.f)); rt->FillRectangle(c, brush); }  // whole cell clickable
    if (!file) {
        const bool isOpen = s.style == StackStyle::InPlace ? s.expanded.count(st->name) > 0 : (s.open >= 0 && &s.stacks[static_cast<size_t>(s.open)] == st);
        if (isOpen) { brush->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * alpha)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 8, c.top + 2, c.right - 8, c.top + kPile + 12), 10, 10), brush); }
        const D2D1_RECT_F pile = D2D1::RectF((c.left + c.right - kPile) / 2, c.top + 6, (c.left + c.right + kPile) / 2, c.top + 6 + kPile);
        const float angles[3] = {-7.f, 6.f, 0.f};
        const int layers = (std::min)(3, static_cast<int>(st->files.size()));
        for (int k = layers - 1; k >= 0; --k) {  // back to front; the newest file on top
            const Image* img = s.thumbs.Get(st->files[static_cast<size_t>(k)].path, pilePx);
            const float shrink = 4.f * k;
            const D2D1_RECT_F box = D2D1::RectF(pile.left + shrink, pile.top + shrink, pile.right - shrink, pile.bottom - shrink);
            PaintImage(rt, img, box, (isOpen ? 0.55f : 1.f) * alpha, layers > 1 ? angles[(3 - layers + k) % 3] : 0.f, Opaque(img) && !st->files[static_cast<size_t>(k)].dir, true);
        }
        LabelShadowed(rt, isOpen && s.style == StackStyle::InPlace ? st->name + L" \u2304" : st->name, TwoLine(12.f), D2D1::RectF(c.left + 2, c.top + kPile + 12, c.right - 2, c.bottom - 2));
        return;
    }
    // A file of an expanded stack: icon or thumbnail plus its name, like a desktop icon. Selection uses the macOS
    // look: a soft plate behind the icon and an accent pill behind the name.
    const bool sel = interactive && s.sel.count(file->path) > 0;
    const float size = (std::min)(kIcon, (c.right - c.left) * 0.6f);
    const D2D1_RECT_F icon = D2D1::RectF((c.left + c.right - size) / 2, c.top + 8 + (kIcon - size) / 2, (c.left + c.right + size) / 2, c.top + 8 + (kIcon + size) / 2);
    if (sel) { brush->SetColor(D2D1::ColorF(0, 0, 0, 0.28f)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(icon.left - 4, icon.top - 4, icon.right + 4, icon.bottom + 4), 8, 8), brush); }
    const Image* img = s.thumbs.Get(file->path, iconPx);
    PaintImage(rt, img, icon, alpha, 0.f, Opaque(img) && !file->dir, true);
    if (alpha < 0.6f) return;  // names appear once a flying file has nearly landed
    const D2D1_RECT_F label = D2D1::RectF(c.left + 3, c.top + kIcon + 14, c.right - 3, c.bottom - 2);
    if (sel) {
        const float tw = (std::min)(TextWidth(DisplayName(*file), Font(12.f)) + 10, c.right - c.left - 6);
        brush->SetColor(D2D1::ColorF(0.04f, 0.42f, 0.98f, 1.f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF((c.left + c.right - tw) / 2, label.top - 1, (c.left + c.right + tw) / 2, label.top + 17), 5, 5), brush);
    }
    LabelShadowed(rt, DisplayName(*file), TwoLine(12.f), label);
}

void PaintColumn() {
    auto& s = S();
    if (!s.column) return;
    const bool flying = !s.flights.empty();
    const RECT win = flying ? s.flightWindow : s.columnRect;
    const int w = win.right - win.left, h = win.bottom - win.top;
    ID2D1RenderTarget* rt = s.columnCanvas.Begin(w, h, s.dpi);
    if (!rt) return;
    ComPtr<ID2D1SolidColorBrush> brush; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.f / 255.f), &brush);
    if (!flying) {
        for (const auto& e : s.entries) {
            const auto& st = s.stacks[static_cast<size_t>(e.stack)];
            PaintTile(rt, brush.Get(), &st, e.file < 0 ? nullptr : &st.files[static_cast<size_t>(e.file)], e.rect, 1.f, true);
        }
    } else {
        const float t = static_cast<float>(std::clamp((Now() - s.flightStart) / kFlightSeconds, 0.0, 1.0));
        const float e = 1.f - std::pow(1.f - t, 3.f);  // ease-out
        const float ox = win.left / K(), oy = win.top / K();
        auto local = [&](D2D1_RECT_F r) { return D2D1::RectF(r.left - ox, r.top - oy, r.right - ox, r.bottom - oy); };
        // Stack tiles last, so files fly out from under and back into their pile.
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& f : s.flights) {
                const Entry* en = f.entry >= 0 ? &s.entries[static_cast<size_t>(f.entry)] : nullptr;
                const bool stackTile = en && en->file < 0;
                if ((pass == 0) == stackTile) continue;
                const float alpha = f.fromAlpha + (f.toAlpha - f.fromAlpha) * e;
                const D2D1_RECT_F r = local(Lerp(f.from, f.to, e));
                if (f.isGhost) { PaintTile(rt, brush.Get(), nullptr, &f.ghost, r, alpha, false); continue; }
                const auto& st = s.stacks[static_cast<size_t>(en->stack)];
                PaintTile(rt, brush.Get(), &st, en->file < 0 ? nullptr : &st.files[static_cast<size_t>(en->file)], r, alpha, false);
            }
    }
    s.columnCanvas.End(s.column, POINT{win.left, win.top});
    if (!DevPngDir().empty() && !flying) s.columnCanvas.SavePng(DevPngDir() + L"\\stacks-column.png");
    if (!DevPngDir().empty() && flying) {  // development: one frame halfway through the flight
        static bool saved = false; const double t = (Now() - s.flightStart) / kFlightSeconds;
        if (!saved && t >= 0.35 && t <= 0.7) { saved = true; s.columnCanvas.SavePng(DevPngDir() + L"\\stacks-flight.png"); }
    }
}

// ---- Glass panel mode -----------------------------------------------------------------------------------------
int PanelRows() {
    const auto& s = S();
    if (s.open < 0 || s.open >= static_cast<int>(s.stacks.size())) return 0;
    return static_cast<int>((s.stacks[static_cast<size_t>(s.open)].files.size() + kColumns - 1) / kColumns);
}
D2D1_RECT_F PanelTile(int index) {
    const auto& s = S();
    const int row = index / kColumns - s.scroll, col = index % kColumns;
    const float x = kPanelPad + col * kTileW, y = kHeader + row * kTileH;
    return D2D1::RectF(x, y, x + kTileW, y + kTileH);
}
int VisibleRows() { const auto& s = S(); return (std::max)(1, static_cast<int>(((s.panelRect.bottom - s.panelRect.top) / K() - kHeader - kPanelPad) / kTileH)); }

void LayoutPanel() {
    auto& s = S();
    int cell = -1;
    for (size_t i = 0; i < s.entries.size(); ++i) if (s.entries[i].stack == s.open && s.entries[i].file < 0) cell = static_cast<int>(i);
    if (cell < 0) return;
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromRect(&s.columnRect, MONITOR_DEFAULTTOPRIMARY), &mi);
    const float maxH = (mi.rcWork.bottom - mi.rcWork.top) / K() - 2 * kMargin;
    const int rows = (std::max)(1, PanelRows());
    const float h = (std::min)(maxH, kHeader + rows * kTileH + kPanelPad), w = kColumns * kTileW + 2 * kPanelPad;
    const auto c = s.entries[static_cast<size_t>(cell)].rect;
    int x = s.columnRect.left + Px(c.left) - Px(w) - Px(8);
    int y = s.columnRect.top + Px(c.top);
    y = std::clamp(y, static_cast<int>(mi.rcWork.top) + Px(kMargin), (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - Px(h) - Px(kMargin)));
    x = (std::max)(x, static_cast<int>(mi.rcWork.left));
    s.panelRect = RECT{x, y, x + Px(w), y + Px(h)};
    s.scroll = std::clamp(s.scroll, 0, (std::max)(0, rows - VisibleRows()));
}

void PaintPanel() {
    auto& s = S();
    if (!s.panel.Hwnd() || s.open < 0 || s.open >= static_cast<int>(s.stacks.size())) return;
    const Theme theme = CurrentTheme();
    const float pw = (s.panelRect.right - s.panelRect.left) / K(), ph = (s.panelRect.bottom - s.panelRect.top) / K();
    const D2D1_RECT_F plate = D2D1::RectF(0, 0, pw, ph);
    const auto& st = s.stacks[static_cast<size_t>(s.open)];
    // Liquid glass (the docky material, refracting the wallpaper and widgets below) with the panel content on top.
    // Text uses white on glass in both themes, like docky's Control Center; the fallback plate follows the theme.
    if (!DevPngDir().empty()) s.panel.RequestSnapshot(DevPngDir() + L"\\stacks-panel.bmp");
    s.panel.Render(s.panelRect, s.raised ? HWND_TOP : HWND_BOTTOM, plate, kRadius, [&](ID2D1DeviceContext* dc, bool gpu) {
        const D2D1_COLOR_F ink = gpu ? D2D1::ColorF(1, 1, 1, 0.96f) : theme.ink, secondary = gpu ? D2D1::ColorF(1, 1, 1, 0.66f) : theme.secondary;
        ComPtr<ID2D1SolidColorBrush> brush; dc->CreateSolidColorBrush(theme.tint, &brush);
        if (!gpu) { brush->SetColor(theme.dark ? D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.96f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.97f)); dc->FillRoundedRectangle(D2D1::RoundedRect(plate, kRadius, kRadius), brush.Get()); }
        Text(dc, st.name, Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(kPanelPad + 4, 8, pw - 100, kHeader - 4), ink);
        Text(dc, std::to_wstring(st.files.size()) + (st.files.size() == 1 ? L" item" : L" items"), Font(12.f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_TRAILING),
             D2D1::RectF(pw - 130, 8, pw - kPanelPad - 4, kHeader - 4), secondary);
        dc->PushAxisAlignedClip(D2D1::RectF(0, kHeader - 2, pw, ph - 4), D2D1_ANTIALIAS_MODE_ALIASED);
        brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
        const int iconPx = Px(kTileIcon * 1.15f);
        const int first = s.scroll * kColumns, last = (std::min)(static_cast<int>(st.files.size()), first + (VisibleRows() + 1) * kColumns);
        for (int i = first; i < last; ++i) {
            const auto tile = PanelTile(i);
            const auto& f = st.files[static_cast<size_t>(i)];
            const bool selected = s.sel.count(f.path) > 0;
            if (selected) { brush->SetColor(D2D1::ColorF(0.04f, 0.42f, 0.98f, 0.55f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 3, tile.top + 2, tile.right - 3, tile.bottom - 2), 12, 12), brush.Get()); brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f)); }
            else if (i == s.hoverTile) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 3, tile.top + 2, tile.right - 3, tile.bottom - 2), 12, 12), brush.Get());
            const Image* img = s.thumbs.Get(f.path, iconPx);
            const D2D1_RECT_F icon = D2D1::RectF((tile.left + tile.right - kTileIcon) / 2, tile.top + 6, (tile.left + tile.right + kTileIcon) / 2, tile.top + 6 + kTileIcon);
            PaintImage(dc, img, icon, 1.f, 0.f, Opaque(img) && !f.dir);
            Text(dc, DisplayName(f), TwoLine(11.5f), D2D1::RectF(tile.left + 4, tile.top + 8 + kTileIcon, tile.right - 4, tile.bottom - 2), ink);
        }
        dc->PopAxisAlignedClip();
    });
}

void ClosePanel() {
    auto& s = S();
    if (s.open < 0) return;
    s.open = -1; s.hoverTile = -1; s.scroll = 0; s.sel.clear(); s.anchor.clear();
    if (s.panel.Hwnd()) { KillTimer(s.panel.Hwnd(), 1); s.panelTicker.Stop(); s.panel.Hide(); }
    PaintColumn();
}
void Place(HWND h, const RECT& r, bool show) {
    SetWindowPos(h, S().raised ? HWND_TOP : HWND_BOTTOM, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}
void OpenPanel(int stack) {
    auto& s = S();
    if (s.open == stack) { ClosePanel(); return; }
    s.open = stack; s.scroll = 0; s.hoverTile = -1; s.sel.clear(); s.anchor.clear();
    s.panel.Show();
    LayoutPanel();
    s.panel.InvalidateBackdrop();
    PaintPanel(); PaintColumn();
    if (s.panel.Hwnd()) { SetTimer(s.panel.Hwnd(), 1, 50, nullptr); s.panelTicker.Start(s.panel.Hwnd(), docky::vblank::kMsgVBlank); }
}
bool ReducedMotion() { BOOL on = TRUE; return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) && !on; }
void ToggleInPlace(int stack) {
    auto& s = S();
    const std::wstring name = s.stacks[static_cast<size_t>(stack)].name;
    // Old cells by identity (stack name, file path), absolute DIP.
    std::map<std::wstring, D2D1_RECT_F> before; std::vector<std::pair<FileEntry, D2D1_RECT_F>> oldFiles;
    const RECT oldWindow = s.flights.empty() ? s.columnRect : s.flightWindow;
    for (const auto& e : s.entries) {
        const auto& st = s.stacks[static_cast<size_t>(e.stack)];
        const std::wstring key = e.file < 0 ? L"stack|" + st.name : L"file|" + st.files[static_cast<size_t>(e.file)].path;
        before[key] = Absolute(e.rect);
        if (e.file >= 0 && st.name == name) oldFiles.push_back({st.files[static_cast<size_t>(e.file)], Absolute(e.rect)});
    }
    const bool opening = !s.expanded.erase(name);
    if (opening) s.expanded.insert(name);
    s.sel.clear(); s.anchor.clear();
    LayoutColumn();
    s.flights.clear();
    if (ReducedMotion()) { Place(s.column, s.columnRect, true); PaintColumn(); return; }
    D2D1_RECT_F stackTo{}, stackFrom{};
    for (size_t i = 0; i < s.entries.size(); ++i) {
        const auto& e = s.entries[i];
        const auto& st = s.stacks[static_cast<size_t>(e.stack)];
        const std::wstring key = e.file < 0 ? L"stack|" + st.name : L"file|" + st.files[static_cast<size_t>(e.file)].path;
        Flight f; f.entry = static_cast<int>(i); f.to = Absolute(e.rect);
        auto old = before.find(key);
        if (old != before.end()) f.from = old->second;
        else { f.from = before.count(L"stack|" + name) ? before[L"stack|" + name] : f.to; f.fromAlpha = 0; }  // flies out of the pile
        if (e.file < 0 && st.name == name) { stackTo = f.to; stackFrom = f.from; }
        s.flights.push_back(f);
    }
    if (!opening) for (const auto& [file, rect] : oldFiles) {  // closing: the files fly back into the pile and fade
        Flight f; f.isGhost = true; f.ghost = file; f.from = rect; f.to = stackTo; f.toAlpha = 0; s.flights.push_back(f);
    }
    (void)stackFrom;
    RECT u{}; UnionRect(&u, &oldWindow, &s.columnRect);
    s.flightWindow = u; s.flightStart = Now();
    Place(s.column, s.flightWindow, true);
    PaintColumn();
    s.flightTicker.Start(s.column, docky::vblank::kMsgVBlank);
}
void StepFlights() {
    auto& s = S();
    if (s.flights.empty()) { s.flightTicker.Stop(); return; }
    if (Now() - s.flightStart >= kFlightSeconds) { s.flights.clear(); s.flightTicker.Stop(); Place(s.column, s.columnRect, true); }
    PaintColumn();
}
int EntryAt(POINT client) {
    const auto& s = S();
    const D2D1_POINT_2F p{client.x / K(), client.y / K()};
    for (size_t i = 0; i < s.entries.size(); ++i) { const auto& c = s.entries[i].rect; if (p.x >= c.left && p.x < c.right && p.y >= c.top && p.y < c.bottom) return static_cast<int>(i); }
    return -1;
}
int TileAt(POINT client) {
    auto& s = S();
    if (s.open < 0) return -1;
    const D2D1_POINT_2F p{client.x / K(), client.y / K()};
    if (p.y < kHeader) return -1;
    const int count = static_cast<int>(s.stacks[static_cast<size_t>(s.open)].files.size());
    for (int i = s.scroll * kColumns; i < count; ++i) { const auto t = PanelTile(i); if (p.x >= t.left && p.x < t.right && p.y >= t.top && p.y < t.bottom) return i; }
    return -1;
}
const FileEntry* FileAt(int tile) {
    auto& s = S();
    if (s.open < 0 || tile < 0) return nullptr;
    const auto& files = s.stacks[static_cast<size_t>(s.open)].files;
    return tile < static_cast<int>(files.size()) ? &files[static_cast<size_t>(tile)] : nullptr;
}
bool DragStarted(POINT pt) { const auto& s = S(); return std::abs(pt.x - s.press.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - s.press.y) > GetSystemMetrics(SM_CYDRAG); }
void KeepBottom(LPARAM l) { if (!S().raised) { auto* wp = reinterpret_cast<WINDOWPOS*>(l); if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = HWND_BOTTOM; } }

// QuickLook (third-party previewer) takes a path on its command line and toggles its preview for it. Found from its
// running process, else its usual install folders. Without it, Space does nothing.
std::wstring QuickLookExe() {
    std::wstring found;
    DWORD pids[2048]; DWORD bytes = 0;
    if (EnumProcesses(pids, sizeof(pids), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(DWORD) && found.empty(); ++i)
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i])) {
                wchar_t path[MAX_PATH * 2]{}; DWORD n = ARRAYSIZE(path);
                if (QueryFullProcessImageNameW(p, 0, path, &n)) { const wchar_t* base = wcsrchr(path, L'\\'); if (base && _wcsicmp(base + 1, L"QuickLook.exe") == 0) found = path; }
                CloseHandle(p);
            }
    if (!found.empty()) return found;
    wchar_t buf[MAX_PATH]{};
    for (const wchar_t* var : {L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        if (!GetEnvironmentVariableW(var, buf, MAX_PATH)) continue;
        for (const wchar_t* tail : {L"\\Programs\\QuickLook\\QuickLook.exe", L"\\QuickLook\\QuickLook.exe"}) {
            const std::wstring candidate = std::wstring(buf) + tail;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
        }
    }
    return {};
}
// QuickLook listens on a per-user pipe "QuickLook.App.Pipe.<user SID>" for lines "<message>|<path>" (verified
// 2026-11-11 with the Microsoft Store build 4.5: Toggle opens/closes the preview, Switch retargets an open one, Close
// closes it). The Store build cannot be started by path, so the pipe is the primary route; a command-line launch is
// the fallback for other installs.
bool QuickLookPipe(const wchar_t* message, const std::wstring& path) {
    static std::wstring pipe;
    if (pipe.empty()) {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            BYTE buffer[256]{}; DWORD size = 0;
            if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
                LPWSTR sid = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) { pipe = std::wstring(L"\\\\.\\pipe\\QuickLook.App.Pipe.") + sid; LocalFree(sid); }
            }
            CloseHandle(token);
        }
    }
    if (pipe.empty()) return false;
    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring line = std::wstring(L"QuickLook.App.PipeMessages.") + message + L"|" + path + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    const bool ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
    CloseHandle(h);
    return ok;
}
void QuickLook(const std::wstring& path) {
    if (QuickLookPipe(L"Toggle", path)) return;
    static std::wstring exe = QuickLookExe();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = QuickLookExe();
    if (exe.empty()) return;
    std::wstring cmd = L"\"" + exe + L"\" \"" + path + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
int EntryOfPath(const std::wstring& path) {
    const auto& s = S();
    for (size_t i = 0; i < s.entries.size(); ++i) if (const FileEntry* f = EntryFile(static_cast<int>(i)); f && f->path == path) return static_cast<int>(i);
    return -1;
}
int SelectedEntry() {
    const auto& s = S();
    if (!s.anchor.empty() && s.sel.count(s.anchor)) return EntryOfPath(s.anchor);
    for (const auto& p : s.sel) if (const int i = EntryOfPath(p); i >= 0) return i;
    return -1;
}
// Selected paths in desktop order (the order Enter opens them and drags carry them).
std::vector<std::wstring> SelectedPaths() {
    const auto& s = S();
    std::vector<std::wstring> out;
    if (s.open >= 0 && s.open < static_cast<int>(s.stacks.size())) { for (const auto& f : s.stacks[static_cast<size_t>(s.open)].files) if (s.sel.count(f.path)) out.push_back(f.path); return out; }
    for (size_t i = 0; i < s.entries.size(); ++i) if (const FileEntry* f = EntryFile(static_cast<int>(i)); f && s.sel.count(f->path)) out.push_back(f->path);
    return out;
}
// Click on a file: plain = only it, Ctrl = toggle it, Shift = the range from the anchor (Ctrl+Shift adds the range).
// `order` lists the paths in the order a Shift-range follows.
void ClickSelect(const std::wstring& path, bool ctrl, bool shift, const std::vector<std::wstring>& order) {
    auto& s = S();
    if (shift && !s.anchor.empty()) {
        auto a = std::find(order.begin(), order.end(), s.anchor), b = std::find(order.begin(), order.end(), path);
        if (a != order.end() && b != order.end()) {
            if (!ctrl) s.sel.clear();
            if (a > b) std::swap(a, b);
            for (auto it = a; it <= b; ++it) s.sel.insert(*it);
            return;
        }
    }
    if (ctrl) { if (!s.sel.erase(path)) s.sel.insert(path); }
    else { s.sel.clear(); s.sel.insert(path); }
    s.anchor = path;
}
std::vector<std::wstring> DesktopOrder() {
    const auto& s = S(); std::vector<std::wstring> out;
    for (size_t i = 0; i < s.entries.size(); ++i) if (const FileEntry* f = EntryFile(static_cast<int>(i))) out.push_back(f->path);
    return out;
}
std::vector<std::wstring> PanelOrder() {
    const auto& s = S(); std::vector<std::wstring> out;
    if (s.open >= 0 && s.open < static_cast<int>(s.stacks.size())) for (const auto& f : s.stacks[static_cast<size_t>(s.open)].files) out.push_back(f.path);
    return out;
}
void OpenPaths(const std::vector<std::wstring>& paths) { for (const auto& p : paths) ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void Recycle(HWND owner, const std::vector<std::wstring>& paths) {
    ComPtr<IFileOperation> op;
    if (paths.empty() || FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return;
    op->SetOwnerWindow(owner); op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE);
    for (const auto& path : paths) { ComPtr<IShellItem> item; if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) op->DeleteItem(item.Get(), nullptr); }
    op->PerformOperations();
}
// Keys while a file is selected (the column takes the keyboard focus when a file is clicked, like the desktop):
// Space Quick Look, Enter open, Delete to the Recycle Bin, arrows move the selection, Esc clears it.
bool OnKey(HWND h, WPARAM key) {
    auto& s = S();
    if (key == 'A' && GetKeyState(VK_CONTROL) < 0) { s.sel.clear(); for (const auto& p : DesktopOrder()) s.sel.insert(p); if (!s.sel.empty() && s.anchor.empty()) s.anchor = *s.sel.begin(); PaintColumn(); return true; }
    const int sel = SelectedEntry();
    if (sel < 0) return false;
    const FileEntry* f = EntryFile(sel);
    if (key == VK_SPACE) { QuickLook(f->path); return true; }
    if (key == VK_RETURN) { OpenPaths(SelectedPaths()); return true; }
    if (key == VK_DELETE) { Recycle(h, SelectedPaths()); return true; }
    if (key == VK_ESCAPE) { QuickLookPipe(L"Close", L""); s.sel.clear(); s.anchor.clear(); PaintColumn(); return true; }
    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        const auto& cur = s.entries[static_cast<size_t>(sel)].rect;
        const float dx = key == VK_LEFT ? -kCellW : key == VK_RIGHT ? kCellW : 0, dy = key == VK_UP ? -kCellH : key == VK_DOWN ? kCellH : 0;
        const float cx = (cur.left + cur.right) / 2 + dx, cy = (cur.top + cur.bottom) / 2 + dy;
        for (size_t i = 0; i < s.entries.size(); ++i) {
            const auto& r = s.entries[i].rect;
            if (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom) { if (const FileEntry* n = EntryFile(static_cast<int>(i))) {
                if (GetKeyState(VK_SHIFT) < 0) s.sel.insert(n->path); else { s.sel.clear(); s.sel.insert(n->path); }
                s.anchor = n->path; PaintColumn(); QuickLookPipe(L"Switch", n->path); } break; }
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK ColumnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto& s = S();
    if (m == docky::vblank::kMsgVBlank) { s.flightTicker.Ack(); StepFlights(); return 0; }
    if (!s.flights.empty() && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) return 0;  // tiles are moving
    switch (m) {
    case WM_MOUSEACTIVATE: {  // clicking a file takes the keyboard focus (Space for Quick Look); stacks do not
        POINT pt{}; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return EntryFile(EntryAt(pt)) ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    case WM_KEYDOWN: if (OnKey(h, w)) return 0; break;
    case WM_WINDOWPOSCHANGING: KeepBottom(l); break;
    case WM_LBUTTONDOWN: s.press = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; s.pressEntry = EntryAt(s.press); s.pressed = s.pressEntry >= 0; if (s.pressed) SetCapture(h); return 0;
    case WM_MOUSEMOVE:
        if (s.draggingStack) {  // free placement: the stack follows the pointer cell by cell
            POINT pt{}; GetCursorPos(&pt);
            const POINT cell = CellAtScreen(pt);
            bool taken = false;
            for (const auto& e : s.entries) if (e.file < 0 && s.stacks[static_cast<size_t>(e.stack)].name != s.dragName && e.cell.x == cell.x && e.cell.y == cell.y) taken = true;
            auto it = s.cells.find(s.dragName);
            if (!taken && (it == s.cells.end() || it->second.x != cell.x || it->second.y != cell.y)) {
                s.cells[s.dragName] = cell; LayoutColumn(); Place(s.column, s.columnRect, true); PaintColumn();
                if (s.open >= 0) { LayoutPanel(); s.panel.InvalidateBackdrop(); PaintPanel(); }
            }
            return 0;
        }
        if (s.pressed && (w & MK_LBUTTON) && DragStarted(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)})) {
            s.pressed = false;
            const int pe = s.pressEntry;
            if (s.freePlace && pe >= 0 && pe < static_cast<int>(s.entries.size()) && s.entries[static_cast<size_t>(pe)].file < 0) {
                s.draggingStack = true; s.dragName = s.stacks[static_cast<size_t>(s.entries[static_cast<size_t>(pe)].stack)].name;
                for (const auto& e : s.entries) if (e.file < 0) s.cells[s.stacks[static_cast<size_t>(e.stack)].name] = e.cell;  // pin every stack where it is
                return 0;  // keeps the capture
            }
            ReleaseCapture();
            if (const FileEntry* f = EntryFile(pe)) {
                if (!s.sel.count(f->path)) { s.sel.clear(); s.sel.insert(f->path); s.anchor = f->path; }
                const auto paths = SelectedPaths(); PaintColumn(); DragOutMany(h, paths); GetStacks().Rescan();
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        if (s.draggingStack) {
            s.draggingStack = false; ReleaseCapture();
            if (s.cellsChanged) s.cellsChanged(s.cells);
            return 0;
        }
        const bool was = s.pressed; s.pressed = false; ReleaseCapture();
        const int index = EntryAt(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (!was || index != s.pressEntry || index < 0) return 0;
        const auto e = s.entries[static_cast<size_t>(index)];
        if (e.file < 0) { if (s.style == StackStyle::InPlace) ToggleInPlace(e.stack); else OpenPanel(e.stack); }
        else if (const FileEntry* f = EntryFile(index)) { ClickSelect(f->path, (w & MK_CONTROL) != 0, (w & MK_SHIFT) != 0, DesktopOrder()); PaintColumn(); }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (const FileEntry* f = EntryFile(EntryAt(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}))) {
            // Like Explorer: double-clicking one of several selected files opens all of them.
            if (s.sel.count(f->path)) OpenPaths(SelectedPaths()); else { const std::wstring path = f->path; OpenPaths({path}); }
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (const FileEntry* f = EntryFile(EntryAt(pt))) {
            if (!s.sel.count(f->path)) { s.sel.clear(); s.sel.insert(f->path); s.anchor = f->path; }
            const auto paths = SelectedPaths(); PaintColumn(); ClientToScreen(h, &pt); ShellMenuMany(s.host, paths, pt); GetStacks().Rescan();
        }
        return 0;
    }
    case kThumbReady: PaintColumn(); if (s.open >= 0) PaintPanel(); return 0;
    case kRescanMsg: KillTimer(h, 2); SetTimer(h, 2, 400, nullptr); return 0;  // debounce bursts of file changes
    case WM_TIMER:
        if (w == 2) { KillTimer(h, 2); GetStacks().Rescan(); }
        else if (w == 4) StepFlights();
        else if (w >= 100 && w < 200) { KillTimer(h, w); const int i = static_cast<int>(w - 100); if (i < static_cast<int>(s.stacks.size())) { if (s.style == StackStyle::InPlace) ToggleInPlace(i); else OpenPanel(i); } }
        return 0;
    case WM_DPICHANGED: case WM_DISPLAYCHANGE: GetStacks().Relayout(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto& s = S();
    if (m == docky::vblank::kMsgVBlank) {
        s.panelTicker.Ack();
        if (s.open < 0) { s.panelTicker.Stop(); return 0; }
        if (s.panel.GlassChanged()) PaintPanel(); else if (s.panel.RefreshLiveBackdrop()) s.panel.Redraw();
        return 0;
    }
    switch (m) {
    case WM_MOUSEACTIVATE: {  // a click on a file takes the keyboard (Space, Enter, Delete, Ctrl+A); elsewhere it does not
        POINT pt{}; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return FileAt(TileAt(pt)) ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    case WM_KEYDOWN: {
        const auto order = PanelOrder();
        if (w == 'A' && GetKeyState(VK_CONTROL) < 0) { s.sel.clear(); for (const auto& p : order) s.sel.insert(p); PaintPanel(); return 0; }
        const auto paths = SelectedPaths();
        if (w == VK_RETURN && !paths.empty()) { OpenPaths(paths); ClosePanel(); return 0; }
        if (w == VK_DELETE && !paths.empty()) { Recycle(h, paths); return 0; }
        if (w == VK_SPACE && !paths.empty()) { QuickLook(s.anchor.empty() ? paths[0] : s.anchor); return 0; }
        break;
    }
    case WM_WINDOWPOSCHANGING: KeepBottom(l); break;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (s.pressed && (w & MK_LBUTTON) && DragStarted(pt)) {
            s.pressed = false;
            if (const FileEntry* f = FileAt(s.pressTile)) {
                if (!s.sel.count(f->path)) { s.sel.clear(); s.sel.insert(f->path); s.anchor = f->path; }
                const auto paths = SelectedPaths(); ReleaseCapture(); PaintPanel(); DragOutMany(h, paths); GetStacks().Rescan();
            }
            return 0;
        }
        const int tile = TileAt(pt);
        if (tile != s.hoverTile) { s.hoverTile = tile; PaintPanel(); }
        TRACKMOUSEEVENT ev{sizeof(ev), TME_LEAVE, h, 0}; TrackMouseEvent(&ev);
        return 0;
    }
    case WM_MOUSELEAVE: if (s.hoverTile != -1) { s.hoverTile = -1; PaintPanel(); } return 0;
    case WM_LBUTTONDOWN: s.press = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; s.pressTile = TileAt(s.press); s.pressed = s.pressTile >= 0; if (s.pressed) SetCapture(h); return 0;
    case WM_LBUTTONUP: {  // a click selects (Ctrl / Shift like the desktop); a double click opens
        const bool wasPressed = s.pressed; s.pressed = false; ReleaseCapture();
        if (!wasPressed) return 0;
        const int tile = TileAt(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (const FileEntry* f = FileAt(tile); f && tile == s.pressTile) { ClickSelect(f->path, (w & MK_CONTROL) != 0, (w & MK_SHIFT) != 0, PanelOrder()); PaintPanel(); }
        else if (!f && !(w & (MK_CONTROL | MK_SHIFT))) { s.sel.clear(); PaintPanel(); }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (const FileEntry* f = FileAt(TileAt(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}))) {
            std::vector<std::wstring> paths = s.sel.count(f->path) ? SelectedPaths() : std::vector<std::wstring>{f->path};
            OpenPaths(paths); ClosePanel();
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (const FileEntry* f = FileAt(TileAt(pt))) {
            if (!s.sel.count(f->path)) { s.sel.clear(); s.sel.insert(f->path); s.anchor = f->path; PaintPanel(); }
            const auto paths = SelectedPaths(); ClientToScreen(h, &pt); ShellMenuMany(s.host, paths, pt); GetStacks().Rescan();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int rows = PanelRows(), next = std::clamp(s.scroll + (GET_WHEEL_DELTA_WPARAM(w) < 0 ? 1 : -1), 0, (std::max)(0, rows - VisibleRows()));
        if (next != s.scroll) { s.scroll = next; s.hoverTile = -1; PaintPanel(); }
        return 0;
    }
    case WM_TIMER:
        if (w == 1) {  // close on a click elsewhere or Esc (the panel never takes focus)
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            POINT pt{}; GetCursorPos(&pt);
            RECT pr = s.panelRect, cr = s.columnRect;
            if ((down && !s.mouseWasDown && !PtInRect(&pr, pt) && !PtInRect(&cr, pt)) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) ClosePanel();
            s.mouseWasDown = down;
        }
        return 0;
    case kThumbReady: PaintPanel(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- Click catcher (selection rectangle on the bare wallpaper) ------------------------------------------------------------
constexpr wchar_t kCatcherClass[] = L"DockyDesktopStacksCatcher", kBandClass[] = L"DockyDesktopStacksBand";
// Files dropped on the bare wallpaper go to the desktop folder, as before the catcher covered it.
class DesktopDrop : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        target_.Reset();
        ComPtr<IShellFolder> desktop;
        if (SUCCEEDED(SHGetDesktopFolder(&desktop))) desktop->CreateViewObject(S().catcher, IID_PPV_ARGS(&target_));
        if (!target_) { *effect = DROPEFFECT_NONE; return S_OK; }
        return target_->DragEnter(data, keys, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override { if (!target_) { *effect = DROPEFFECT_NONE; return S_OK; } return target_->DragOver(keys, pt, effect); }
    HRESULT STDMETHODCALLTYPE DragLeave() override { if (target_) target_->DragLeave(); target_.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (!target_) { *effect = DROPEFFECT_NONE; return S_OK; }
        const HRESULT hr = target_->Drop(data, keys, pt, effect); target_.Reset();
        if (S().column) PostMessageW(S().column, kRescanMsg, 0, 0);
        return hr;
    }
private:
    std::atomic<ULONG> refs_{1}; ComPtr<IDropTarget> target_;
};
// Keeps the catcher under every other docky Desktop window (they all sit at the bottom of the Z order).
void CatcherToBottom() {
    auto& s = S();
    if (!s.catcher) return;
    DWORD self = GetCurrentProcessId();
    for (HWND w = GetWindow(s.catcher, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (pid == self && IsWindowVisible(w)) { SetWindowPos(s.catcher, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); return; }
    }
}
void PlaceCatcher() {
    auto& s = S();
    if (!s.catcher) return;
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT r = mi.rcWork;
    SetWindowPos(s.catcher, HWND_BOTTOM, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void UpdateBand(POINT now) {
    auto& s = S();
    RECT r{(std::min)(s.bandStart.x, now.x), (std::min)(s.bandStart.y, now.y), (std::max)(s.bandStart.x, now.x) + 1, (std::max)(s.bandStart.y, now.y) + 1};
    if (!s.bandShown && std::abs(now.x - s.bandStart.x) <= GetSystemMetrics(SM_CXDRAG) && std::abs(now.y - s.bandStart.y) <= GetSystemMetrics(SM_CYDRAG)) return;
    if (s.band) {
        HWND above = s.column ? GetWindow(s.column, GW_HWNDPREV) : nullptr;  // just above the stacks
        SetWindowPos(s.band, above ? above : HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(s.band, nullptr, TRUE);
    }
    s.bandShown = true;
    std::set<std::wstring> next = s.bandBase;
    for (size_t i = 0; i < s.entries.size(); ++i) {
        const FileEntry* f = EntryFile(static_cast<int>(i)); if (!f) continue;
        const RECT e = EntryScreenRect(s.entries[i]); RECT hit{};
        if (IntersectRect(&hit, &e, &r)) next.insert(f->path);
    }
    if (next != s.sel) { s.sel = std::move(next); PaintColumn(); }
}
LRESULT CALLBACK BandProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r{}; GetClientRect(h, &r);
        HBRUSH fill = CreateSolidBrush(RGB(160, 196, 255)); FillRect(dc, &r, fill); DeleteObject(fill);
        HBRUSH edge = CreateSolidBrush(RGB(235, 242, 255)); FrameRect(dc, &r, edge); DeleteObject(edge);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK CatcherProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto& s = S();
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: { auto* wp = reinterpret_cast<WINDOWPOS*>(l); if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = HWND_BOTTOM; break; }
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &pt);
        s.banding = true; s.bandShown = false; s.bandStart = pt;
        const bool ctrl = (w & MK_CONTROL) != 0;
        s.bandBase = ctrl ? s.sel : std::set<std::wstring>{};
        if (!ctrl && !s.sel.empty()) { s.sel.clear(); s.anchor.clear(); PaintColumn(); }
        SetCapture(h); return 0;
    }
    case WM_MOUSEMOVE:
        if (s.banding && (w & MK_LBUTTON)) { POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &pt); UpdateBand(pt); }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (s.banding) {
            s.banding = false; if (m == WM_LBUTTONUP) ReleaseCapture();
            if (s.band) ShowWindow(s.band, SW_HIDE);
            if (!s.sel.empty()) { const int e = SelectedEntry(); if (const FileEntry* f = EntryFile(e)) s.anchor = f->path; if (s.column) SetForegroundWindow(s.column); }
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &pt);
        if (!s.sel.empty()) { s.sel.clear(); s.anchor.clear(); PaintColumn(); }
        DesktopBackgroundMenu(s.host, pt); GetStacks().Rescan();
        return 0;
    }
    case WM_TIMER: if (w == 1) CatcherToBottom(); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: PlaceCatcher(); break;
    }
    return DefWindowProcW(h, m, w, l);
}
void CreateCatcher(HINSTANCE instance) {
    auto& s = S();
    WNDCLASSW wc{}; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = CatcherProc; wc.lpszClassName = kCatcherClass; RegisterClassW(&wc);
    wc.lpfnWndProc = BandProc; wc.lpszClassName = kBandClass; RegisterClassW(&wc);
    // Alpha 1 of 255: invisible, but hit-testable, so clicks on the bare wallpaper reach it.
    s.catcher = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCatcherClass, L"DockOver Desktop Stacks selection", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    s.band = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBandClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (s.band) SetLayeredWindowAttributes(s.band, 0, 90, LWA_ALPHA);
    if (!s.catcher) return;
    SetLayeredWindowAttributes(s.catcher, 0, 1, LWA_ALPHA);
    RegisterDragDrop(s.catcher, new DesktopDrop());
    PlaceCatcher();
    SetTimer(s.catcher, 1, 1000, nullptr);
}
void DestroyCatcher() {
    auto& s = S();
    if (s.catcher) { RevokeDragDrop(s.catcher); DestroyWindow(s.catcher); s.catcher = nullptr; }
    if (s.band) { DestroyWindow(s.band); s.band = nullptr; }
}

void Watch() {
    auto& s = S();
    std::vector<HANDLE> handles{s.stopWatch};
    for (const auto& folder : DesktopFolders()) {
        HANDLE c = FindFirstChangeNotificationW(folder.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (c != INVALID_HANDLE_VALUE) handles.push_back(c);
    }
    for (;;) {
        const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        const size_t i = r - WAIT_OBJECT_0;
        if (i < handles.size()) { FindNextChangeNotification(handles[i]); if (s.column) PostMessageW(s.column, kRescanMsg, 0, 0); }
    }
    for (size_t i = 1; i < handles.size(); ++i) FindCloseChangeNotification(handles[i]);
}
}  // namespace

bool Stacks::Start(HWND host, GroupBy by, StackStyle style) {
    auto& s = S();
    if (s.running) return true;
    s.host = host; s.by = by; s.style = style;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ColumnProc; wc.lpszClassName = kColumnClass; RegisterClassW(&wc);
    s.column = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kColumnClass, L"DockOver Desktop Stacks", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!s.column || !s.panel.Create(kPanelClass, PanelProc, 0, false)) return false;
    // The glass panel looks like the menu bar popups (Glass Studio > Menu bar popups) and refracts the live desktop.
    s.panel.SetFrosted(true); s.panel.SetLiveBackdrop(true);
    CreateCatcher(instance);
    RegisterLayer(s.column, &s.columnCanvas);  // the panel's glass refracts the stacks under it too
    s.thumbs.Start(s.column);
    s.stopWatch = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.watcher = std::thread(Watch);
    s.running = true;
    Rescan();
    return true;
}

void Stacks::Stop() {
    auto& s = S();
    if (!s.running) return;
    s.running = false;
    if (s.stopWatch) SetEvent(s.stopWatch);
    if (s.watcher.joinable()) s.watcher.join();
    if (s.stopWatch) { CloseHandle(s.stopWatch); s.stopWatch = nullptr; }
    s.thumbs.Stop();
    s.panel.Hide();
    DestroyCatcher();
    if (s.column) { UnregisterLayer(s.column); DestroyWindow(s.column); }
    s.column = nullptr; s.open = -1; s.stacks.clear(); s.entries.clear();
}

bool Stacks::Running() const { return S().running; }
void Stacks::SetGroupBy(GroupBy by) { S().by = by; if (S().running) { ClosePanel(); S().expanded.clear(); Rescan(); } }
void Stacks::SetStyle(StackStyle style) { auto& s = S(); s.style = style; if (s.running) { ClosePanel(); s.expanded.clear(); s.sel.clear(); Relayout(); } }
void Stacks::SetFreePlacement(bool on) { auto& s = S(); if (s.freePlace == on) return; s.freePlace = on; if (s.running) Relayout(); }
void Stacks::SetCells(const std::map<std::wstring, POINT>& cells) { S().cells = cells; if (S().running && S().freePlace) Relayout(); }
void Stacks::OnCellsChanged(std::function<void(const std::map<std::wstring, POINT>&)> changed) { S().cellsChanged = std::move(changed); }

void Stacks::Rescan() {
    auto& s = S();
    if (!s.running) return;
    FILETIME now{}; GetSystemTimeAsFileTime(&now); FILETIME local{}; FileTimeToLocalFileTime(&now, &local);
    const std::wstring openName = s.open >= 0 && s.open < static_cast<int>(s.stacks.size()) ? s.stacks[static_cast<size_t>(s.open)].name : L"";
    s.stacks = BuildStacks(ScanDesktop(), s.by, (static_cast<ULONGLONG>(local.dwHighDateTime) << 32) | local.dwLowDateTime);
    s.thumbs.Clear(); s.bitmaps.clear(); s.flights.clear();
    { std::set<std::wstring> still; for (const auto& st : s.stacks) for (const auto& f : st.files) if (s.sel.count(f.path)) still.insert(f.path); s.sel = std::move(still); }
    s.open = -1;
    for (size_t i = 0; i < s.stacks.size(); ++i) if (s.stacks[i].name == openName) s.open = static_cast<int>(i);
    Relayout();
}

void Stacks::Relayout() {
    auto& s = S();
    if (!s.running) return;
    LayoutColumn();
    if (s.stacks.empty()) ShowWindow(s.column, SW_HIDE); else Place(s.column, s.columnRect, true);
    PaintColumn();
    PlaceCatcher(); CatcherToBottom();
    if (s.open >= 0) { LayoutPanel(); s.panel.InvalidateBackdrop(); PaintPanel(); } else s.panel.Hide();
}

void Stacks::Raise(bool raised) {
    auto& s = S();
    s.raised = raised;
    if (!s.running) return;
    for (HWND h : {s.column, s.panel.Hwnd()}) if (h && IsWindowVisible(h)) SetWindowPos(h, raised ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Stacks::DevOpen(int index) {
    auto& s = S();
    if (!s.running || index < 0 || index >= static_cast<int>(s.stacks.size())) return;
    SetTimer(s.column, 100 + static_cast<UINT_PTR>(index), 1500, nullptr);  // after the thumbnails have loaded
}
Stacks& GetStacks() { static Stacks s; return s; }

namespace {
HWND DesktopListView() {
    HWND found = nullptr;
    EnumWindows([](HWND top, LPARAM l) -> BOOL {
        HWND view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
        if (view) { *reinterpret_cast<HWND*>(l) = FindWindowExW(view, nullptr, L"SysListView32", nullptr); return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
bool g_iconsHidden = false;
bool g_shortcutHidden = false;  // hidden by the Hide DockOver Desktop shortcut (they were showing before)
void ApplyIconVisibility() {
    if (HWND list = DesktopListView()) {
        const bool hide = g_iconsHidden || g_shortcutHidden, visible = IsWindowVisible(list) != FALSE;
        if (visible == hide) ShowWindow(list, hide ? SW_HIDE : SW_SHOW);
    }
}
}
// Explorer's desktop icon view is hidden with ShowWindow only (no setting is changed). It is re-checked every few
// seconds because Explorer recreates or re-shows the view (Explorer restart, "Show desktop icons", resolution change).
void HideDesktopIcons(bool hide) {
    g_iconsHidden = hide;
    ApplyIconVisibility();
}
void HideDesktopIconsForShortcut(bool hide) {
    if (hide) { HWND list = DesktopListView(); if (!list || !IsWindowVisible(list)) return; g_shortcutHidden = true; }  // only icons that were showing
    else if (!g_shortcutHidden) return;
    else g_shortcutHidden = false;
    ApplyIconVisibility();
}
void MaintainDesktopIcons() { if (g_iconsHidden || g_shortcutHidden) ApplyIconVisibility(); }

}  // namespace docky::desk
