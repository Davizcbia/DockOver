#include "launchpad_model.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <set>

#include "json_lite.h"

namespace dock::launchpad {
namespace {

using json::Value;

void Dedupe(Layout& layout) {
    std::set<std::string> seen;
    std::vector<Node> nodes;
    for (Node& n : layout.nodes) {
        if (!n.folder) {
            if (!n.appId.empty() && seen.insert(n.appId).second) nodes.push_back(std::move(n));
            continue;
        }
        std::vector<std::string> apps;
        for (std::string& a : n.apps)
            if (!a.empty() && seen.insert(a).second) apps.push_back(std::move(a));
        n.apps = std::move(apps);
        nodes.push_back(std::move(n));
    }
    layout.nodes = std::move(nodes);
}

}  // namespace

void Normalize(Layout& layout) {
    Dedupe(layout);
    std::vector<Node> nodes;
    for (Node& n : layout.nodes) {
        if (n.folder && n.apps.size() < 2) {
            // A folder needs at least two applications (as on macOS); a single one is promoted.
            for (std::string& a : n.apps) {
                Node app;
                app.appId = std::move(a);
                nodes.push_back(std::move(app));
            }
            continue;
        }
        nodes.push_back(std::move(n));
    }
    layout.nodes = std::move(nodes);
}

Layout DefaultLayout(const std::vector<std::string>& discovered) {
    Layout l;
    for (const std::string& id : discovered) {
        Node n;
        n.appId = id;
        l.nodes.push_back(std::move(n));
    }
    Normalize(l);
    return l;
}

Layout Merge(const Layout& saved, const std::vector<std::string>& discovered) {
    const std::set<std::string> installed(discovered.begin(), discovered.end());
    Layout l;
    std::set<std::string> placed;
    for (const Node& n : saved.nodes) {
        if (!n.folder) {
            if (installed.count(n.appId) && placed.insert(n.appId).second) l.nodes.push_back(n);
            continue;
        }
        Node f = n;
        f.apps.clear();
        for (const std::string& a : n.apps)
            if (installed.count(a) && placed.insert(a).second) f.apps.push_back(a);
        l.nodes.push_back(std::move(f));
    }
    const std::set<std::string> hidden(saved.hidden.begin(), saved.hidden.end());
    for (const std::string& id : saved.hidden)
        if (installed.count(id) && !placed.count(id)) l.hidden.push_back(id);
    for (const std::string& id : discovered) {
        if (placed.count(id) || hidden.count(id)) continue;
        Node n;
        n.appId = id;
        l.nodes.push_back(std::move(n));
        placed.insert(id);
    }
    Normalize(l);
    return l;
}

bool MoveNode(Layout& layout, size_t from, size_t to) {
    if (from >= layout.nodes.size() || from == to) return false;
    to = std::min(to, layout.nodes.size() - 1);
    if (from == to) return false;
    Node n = std::move(layout.nodes[from]);
    layout.nodes.erase(layout.nodes.begin() + static_cast<std::ptrdiff_t>(from));
    layout.nodes.insert(layout.nodes.begin() + static_cast<std::ptrdiff_t>(to), std::move(n));
    return true;
}

int DropOnto(Layout& layout, size_t dragged, size_t target, const std::wstring& newFolderName,
             const std::string& newFolderId) {
    if (dragged >= layout.nodes.size() || target >= layout.nodes.size() || dragged == target) return -1;
    if (layout.nodes[dragged].folder) return -1;  // folders cannot be nested
    const std::string app = layout.nodes[dragged].appId;
    Node& t = layout.nodes[target];
    if (t.folder) {
        t.apps.push_back(app);
    } else {
        Node f;
        f.folder = true;
        f.folderId = newFolderId;
        f.folderName = newFolderName;
        f.apps = {t.appId, app};
        t = std::move(f);
    }
    layout.nodes.erase(layout.nodes.begin() + static_cast<std::ptrdiff_t>(dragged));
    return static_cast<int>(dragged < target ? target - 1 : target);
}

bool MoveWithinFolder(Layout& layout, size_t folder, size_t from, size_t to) {
    if (folder >= layout.nodes.size() || !layout.nodes[folder].folder) return false;
    auto& apps = layout.nodes[folder].apps;
    if (from >= apps.size()) return false;
    to = std::min(to, apps.size() - 1);
    if (from == to) return false;
    std::string a = std::move(apps[from]);
    apps.erase(apps.begin() + static_cast<std::ptrdiff_t>(from));
    apps.insert(apps.begin() + static_cast<std::ptrdiff_t>(to), std::move(a));
    return true;
}

bool MoveOutOfFolder(Layout& layout, size_t folder, size_t appIndex, size_t insertAt) {
    if (folder >= layout.nodes.size() || !layout.nodes[folder].folder) return false;
    auto& apps = layout.nodes[folder].apps;
    if (appIndex >= apps.size()) return false;
    Node n;
    n.appId = apps[appIndex];
    apps.erase(apps.begin() + static_cast<std::ptrdiff_t>(appIndex));
    insertAt = std::min(insertAt, layout.nodes.size());
    layout.nodes.insert(layout.nodes.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(n));
    Normalize(layout);
    return true;
}

bool RenameFolder(Layout& layout, size_t folder, const std::wstring& name) {
    if (folder >= layout.nodes.size() || !layout.nodes[folder].folder) return false;
    std::wstring trimmed = name;
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    if (trimmed.empty() || trimmed == layout.nodes[folder].folderName) return false;
    layout.nodes[folder].folderName = trimmed.substr(0, 64);
    return true;
}

std::string ToJson(const Layout& layout) {
    Value root = Value::MakeObject();
    root.Set("format", Value::MakeString("LiquidDockLaunchpad"));
    root.Set("version", Value::MakeNumber(1));
    Value nodes;
    nodes.type = Value::Type::Array;
    for (const Node& n : layout.nodes) {
        Value v = Value::MakeObject();
        if (n.folder) {
            v.Set("folder", Value::MakeString(n.folderId));
            v.Set("name", Value::MakeString(json::ToUtf8(n.folderName)));
            Value apps;
            apps.type = Value::Type::Array;
            for (const std::string& a : n.apps) apps.array.push_back(Value::MakeString(a));
            v.Set("apps", apps);
        } else {
            v.Set("app", Value::MakeString(n.appId));
        }
        nodes.array.push_back(std::move(v));
    }
    root.Set("nodes", nodes);
    if (!layout.hidden.empty()) {
        Value hidden;
        hidden.type = Value::Type::Array;
        for (const std::string& a : layout.hidden) hidden.array.push_back(Value::MakeString(a));
        root.Set("hidden", hidden);
    }
    return json::Write(root);
}

bool FromJson(const std::string& text, Layout& layout, std::wstring& error) {
    Value root;
    std::string perr;
    if (!json::Parse(text, root, perr)) {
        error = L"launchpad layout is not valid JSON: " + json::FromUtf8(perr);
        return false;
    }
    const Value* nodes = root.Find("nodes");
    if (!root.IsObject() || !nodes || nodes->type != Value::Type::Array) {
        error = L"launchpad layout has no node list";
        return false;
    }
    Layout l;
    for (const Value& v : nodes->array) {
        if (!v.IsObject()) {
            error = L"launchpad layout contains an invalid node";
            return false;
        }
        Node n;
        if (const Value* app = v.Find("app"); app && app->type == Value::Type::String) {
            n.appId = app->string;
        } else if (const Value* f = v.Find("folder"); f && f->type == Value::Type::String) {
            n.folder = true;
            n.folderId = f->string;
            if (const Value* name = v.Find("name"); name && name->type == Value::Type::String)
                n.folderName = json::FromUtf8(name->string);
            if (const Value* apps = v.Find("apps"); apps && apps->type == Value::Type::Array)
                for (const Value& a : apps->array)
                    if (a.type == Value::Type::String) n.apps.push_back(a.string);
        } else {
            error = L"launchpad layout node is neither an app nor a folder";
            return false;
        }
        l.nodes.push_back(std::move(n));
    }
    if (const Value* hidden = root.Find("hidden"); hidden && hidden->type == Value::Type::Array)
        for (const Value& a : hidden->array)
            if (a.type == Value::Type::String) l.hidden.push_back(a.string);
    layout = std::move(l);
    return true;
}

bool Load(const std::wstring& path, Layout& layout, std::wstring& error, bool* exists) {
    if (exists) *exists = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return true;  // first run
        error = L"launchpad layout cannot be opened";
        return false;
    }
    if (exists) *exists = true;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string text(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 4 << 20)), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        error = L"launchpad layout cannot be read";
        return false;
    }
    text.resize(read);
    return FromJson(text, layout, error);
}

bool Save(const std::wstring& path, const Layout& layout, std::wstring& error) {
    const std::string text = ToJson(layout);
    const std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot write launchpad layout";
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || written != text.size() || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        error = L"cannot replace launchpad layout";
        return false;
    }
    return true;
}

}  // namespace dock::launchpad
