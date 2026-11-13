#include "menubar_extras.h"
#include "json_lite.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace docky::extras {
namespace {
using dock::json::Value;
std::wstring W(const std::string& s) { return dock::json::FromUtf8(s); }
std::string U(const std::wstring& s) { return dock::json::ToUtf8(s); }
std::string Str(const Value& o, const char* key) { const Value* v = o.Find(key); return v && v->type == Value::Type::String ? v->string : std::string(); }
bool Bool(const Value& o, const char* key, bool fallback) { const Value* v = o.Find(key); return v && v->type == Value::Type::Bool ? v->boolean : fallback; }
bool ValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 128) return false;
    for (wchar_t c : id) if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L'_')) return false;
    return true;
}
std::wstring Clip(std::wstring s, size_t n) { if (s.size() > n) s.resize(n); for (auto& c : s) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' '; return s; }
bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::ostringstream s; s << f.rdbuf(); out = s.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) out.erase(0, 3);
    return out.size() < 1024 * 1024;
}
bool ParseItem(const Value& o, StatusItem& item, bool manifest, std::string& error) {
    item.id = W(Str(o, "id"));
    if (!ValidId(item.id)) { error = "id must be 1-128 characters of A-Z a-z 0-9 . - _"; return false; }
    item.title = Clip(W(Str(o, "title")), 32);
    item.tooltip = Clip(W(Str(o, "tooltip")), 128);
    item.iconPath = W(Str(o, "icon"));
    item.templateIcon = Bool(o, "template", true);
    if (const Value* n = o.Find("notifyHwnd"); n && n->type == Value::Type::Number && n->number > 0)
        item.notify = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(n->number));
    if (manifest) { item.run = W(Str(o, "run")); item.args = W(Str(o, "args")); }
    if (const Value* m = o.Find("menu"); m && m->type == Value::Type::Array) {
        for (const Value& e : m->array) {
            if (item.menu.size() >= 50) break;
            if (!e.IsObject()) continue;
            MenuEntry entry;
            entry.separator = Bool(e, "separator", false);
            entry.id = Clip(W(Str(e, "id")), 64);
            entry.title = Clip(W(Str(e, "title")), 80);
            entry.enabled = Bool(e, "enabled", true);
            entry.checked = Bool(e, "checked", false);
            if (manifest) { entry.run = W(Str(e, "run")); entry.args = W(Str(e, "args")); }
            if (!entry.separator && entry.title.empty()) continue;
            item.menu.push_back(std::move(entry));
        }
    }
    if (item.title.empty() && item.iconPath.empty()) item.title = item.id.substr(0, 12);
    return true;
}
std::string Escape(const std::wstring& w) { return dock::json::Write(Value::MakeString(U(w)), 0); }
}  // namespace

std::wstring ConfigPath() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::wstring(local) + L"\\LiquidDesktop\\menubar-items.json";
}

Config LoadConfig(const std::wstring& path) {
    Config c; std::string text, error; Value root;
    if (path.empty() || !ReadFile(path, text) || !dock::json::Parse(text, root, error) || !root.IsObject()) return c;
    if (const Value* b = root.Find("builtins"); b && b->IsObject()) {
        c.wifi = Bool(*b, "wifi", true); c.sound = Bool(*b, "sound", true);
        c.battery = Bool(*b, "battery", true); c.control = Bool(*b, "controlCenter", true);
    }
    c.hiddenIcons = Bool(root, "hiddenIcons", true);
    c.trayHost = Bool(root, "trayHost", true);
    auto list = [&](const char* key, std::vector<std::wstring>& out) {
        if (const Value* a = root.Find(key); a && a->type == Value::Type::Array)
            for (const Value& v : a->array) if (v.type == Value::Type::String && !v.string.empty() && out.size() < 200) out.push_back(W(v.string));
    };
    list("shown", c.shown); list("hidden", c.hidden);
    return c;
}

bool SaveConfig(const std::wstring& path, const Config& c) {
    if (path.empty()) return false;
    Value root = Value::MakeObject();
    root.Set("version", Value::MakeNumber(1));
    Value b = Value::MakeObject();
    b.Set("wifi", Value::MakeBool(c.wifi)); b.Set("sound", Value::MakeBool(c.sound));
    b.Set("battery", Value::MakeBool(c.battery)); b.Set("controlCenter", Value::MakeBool(c.control));
    root.Set("builtins", std::move(b));
    root.Set("hiddenIcons", Value::MakeBool(c.hiddenIcons));
    root.Set("trayHost", Value::MakeBool(c.trayHost));
    auto list = [](const std::vector<std::wstring>& in) { Value a; a.type = Value::Type::Array; for (const auto& s : in) a.array.push_back(Value::MakeString(U(s))); return a; };
    root.Set("shown", list(c.shown)); root.Set("hidden", list(c.hidden));
    const std::string text = dock::json::Write(root);
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc); if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size())); if (!f) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temp.c_str()); return false; }
    return true;
}

bool IsShown(const Config& c, const std::wstring& id) {
    if (id == L"builtin:wifi") return c.wifi;
    if (id == L"builtin:sound") return c.sound;
    if (id == L"builtin:battery") return c.battery;
    if (id == L"builtin:control") return c.control;
    if (id.rfind(L"tray:", 0) == 0) return std::find(c.shown.begin(), c.shown.end(), id) != c.shown.end();
    if (id.rfind(L"ext:", 0) == 0) return std::find(c.hidden.begin(), c.hidden.end(), id) == c.hidden.end();
    return false;
}

void SetShown(Config& c, const std::wstring& id, bool shown) {
    if (id == L"builtin:wifi") c.wifi = shown;
    else if (id == L"builtin:sound") c.sound = shown;
    else if (id == L"builtin:battery") c.battery = shown;
    else if (id == L"builtin:control") c.control = shown;
    else if (id.rfind(L"tray:", 0) == 0) {
        c.shown.erase(std::remove(c.shown.begin(), c.shown.end(), id), c.shown.end());
        if (shown) c.shown.push_back(id);
    } else if (id.rfind(L"ext:", 0) == 0) {
        c.hidden.erase(std::remove(c.hidden.begin(), c.hidden.end(), id), c.hidden.end());
        if (!shown) c.hidden.push_back(id);
    }
}

bool ParseMessage(const std::string& utf8, Message& out, std::string& error) {
    Value root; out = Message{};
    if (!dock::json::Parse(utf8, root, error)) return false;
    if (!root.IsObject()) { error = "message must be a JSON object"; return false; }
    const std::string type = Str(root, "type");
    if (type == "statusItem.remove") {
        out.item.id = W(Str(root, "id"));
        if (!ValidId(out.item.id)) { error = "invalid id"; return false; }
        out.action = Action::Remove; return true;
    }
    if (type != "statusItem.set") { error = "unsupported type"; return false; }
    if (!ParseItem(root, out.item, false, error)) return false;
    out.action = Action::Set; return true;
}

std::vector<StatusItem> LoadManifests(const std::wstring& root) {
    std::vector<StatusItem> items;
    if (root.empty()) return items;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return items;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring folder = root + L"\\" + fd.cFileName;
        std::string text, error; Value manifest;
        if (!ReadFile(folder + L"\\extension.json", text) || !dock::json::Parse(text, manifest, error) || !manifest.IsObject()) continue;
        const Value* list = manifest.Find("statusItems");
        if (!list || list->type != Value::Type::Array) continue;
        for (const Value& o : list->array) {
            if (!o.IsObject() || items.size() >= 64) continue;
            StatusItem item; std::string why;
            if (!ParseItem(o, item, true, why)) continue;
            item.fromManifest = true; item.folder = folder; item.notify = nullptr;
            if (!item.iconPath.empty() && item.iconPath.find(L':') == std::wstring::npos && item.iconPath.rfind(L"\\\\", 0) != 0)
                item.iconPath = folder + L"\\" + item.iconPath;  // relative to the manifest
            items.push_back(std::move(item));
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return items;
}

std::string ClickedEvent(const std::wstring& id, bool right) {
    return "{\"v\":1,\"type\":\"statusItem.clicked\",\"id\":" + Escape(id) + ",\"button\":\"" + (right ? "right" : "left") + "\"}";
}
std::string MenuEvent(const std::wstring& id, const std::wstring& entryId) {
    return "{\"v\":1,\"type\":\"statusItem.menu\",\"id\":" + Escape(id) + ",\"item\":" + Escape(entryId) + "}";
}

}  // namespace docky::extras
