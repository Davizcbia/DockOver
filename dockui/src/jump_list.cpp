#include "jump_list.h"

#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dock::jump {
namespace {
bool ItemEntry(IShellItem* item, Entry& out) {
    PWSTR name = nullptr, parsing = nullptr;
    const bool ok = SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing));
    if (ok) { out.label = name; out.parsingName = parsing; }
    CoTaskMemFree(name);
    CoTaskMemFree(parsing);
    return ok && !out.label.empty() && !out.parsingName.empty();
}
std::wstring PropString(IPropertyStore* store) {
    std::wstring out;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (store && SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}
bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}
}  // namespace

std::vector<Entry> QuickAccessFolders(size_t max) {
    std::vector<Entry> out;
    ComPtr<IShellItem> folder;
    if (FAILED(SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr, IID_PPV_ARGS(&folder)))) return out;
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) return out;
    ComPtr<IShellItem> item;
    while (out.size() < max && items->Next(1, &item, nullptr) == S_OK) {
        SFGAOF attrs = 0;
        // Folders only (Quick access on Windows 11 can also list files under "Recent").
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)) && (attrs & SFGAO_FOLDER)) {
            Entry e;
            if (ItemEntry(item.Get(), e)) out.push_back(std::move(e));
        }
        item.Reset();
    }
    return out;
}

std::vector<Entry> RecentForApp(const std::wstring& aumid, size_t max) {
    std::vector<Entry> out;
    if (aumid.empty()) return out;
    ComPtr<IApplicationDocumentLists> lists;
    if (FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&lists)))) return out;
    if (FAILED(lists->SetAppID(aumid.c_str()))) return out;
    ComPtr<IObjectArray> array;
    if (FAILED(lists->GetList(ADLT_RECENT, static_cast<UINT>(max), IID_PPV_ARGS(&array)))) return out;
    UINT n = 0;
    array->GetCount(&n);
    for (UINT i = 0; i < n && out.size() < max; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(array->GetAt(i, IID_PPV_ARGS(&item)))) continue;  // links (IShellLink) are skipped
        Entry e;
        if (ItemEntry(item.Get(), e)) out.push_back(std::move(e));
    }
    return out;
}

std::wstring AppIdFor(const PinnedItem& item) {
    if (!item.aumid.empty()) return item.aumid;
    if (EndsWithI(item.target, L".lnk")) {
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(item.target.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
            const std::wstring id = PropString(store.Get());
            if (!id.empty()) return id;
        }
    }
    if (item.window && IsWindow(item.window)) {
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(item.window, IID_PPV_ARGS(&store)))) return PropString(store.Get());
    }
    return {};
}

bool Open(const std::wstring& parsingName, HWND owner) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(parsingName.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpIDList = pidl;
    sei.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&sei) != FALSE;
    CoTaskMemFree(pidl);
    DLOG(L"jump: open '%ls' ok=%d", parsingName.c_str(), ok ? 1 : 0);
    return ok;
}
}  // namespace dock::jump
