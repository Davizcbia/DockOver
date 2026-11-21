#pragma once
// Local music files (Settings > Music > Match local music files).
// Some players publish a track as its file name ("01 - Artist - Song.mp3") or without an artist. Then the file is
// looked for on this PC and its own tags are used to find the cover and lyrics:
//  1. Windows' Recent folder: opening a file in a player leaves "<file name>.lnk" there.
//  2. An index of audio files in Music, Downloads, Desktop and Documents (built once, rebuilt at most once a minute
//     when a name is not found).
// Found: title, artist, album and length come from the file's tags (Windows property system: MP3, FLAC, M4A, WMA ...),
// the cover from its embedded picture (Windows thumbnail), lyrics from "<same name>.lrc" beside it or the lyrics tag.
// Not found: "Artist - Title" is read from the file name. Nothing here uses the network.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <knownfolders.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "propsys.lib")

namespace nowplaying::localtags {

struct Result {
    bool changed = false;  // something was resolved (tags, file name or cover)
    std::wstring title, artist, album, path, lyricsText;  // lyricsText: the file's lyrics tag
    std::string lyricsBytes;                              // or a ".lrc"/".txt" beside the file (raw; decoded by LyricsSource)
    int64_t duration = 0;  // 100 ns, 0 unknown
    std::shared_ptr<const std::vector<uint8_t>> cover; uint32_t coverW = 0, coverH = 0;
};

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
inline std::wstring TrimText(std::wstring s) {
    const auto b = s.find_first_not_of(L" \t\r\n_"); if (b == std::wstring::npos) return {};
    const auto e = s.find_last_not_of(L" \t\r\n_"); return s.substr(b, e - b + 1);
}
inline bool AudioExtension(const std::wstring& ext) {
    static const wchar_t* const kExt[] = {L".mp3", L".flac", L".m4a", L".aac", L".wav", L".ogg", L".opus", L".wma", L".ape", L".alac", L".aiff", L".aif", L".dsf", L".wv"};
    const std::wstring e = Lower(ext);
    return std::any_of(std::begin(kExt), std::end(kExt), [&](const wchar_t* k) { return e == k; });
}
inline std::wstring ExtensionOf(const std::wstring& name) { const auto dot = name.find_last_of(L'.'); return dot == std::wstring::npos ? L"" : name.substr(dot); }
inline std::wstring FileNameOf(const std::wstring& s) { const auto slash = s.find_last_of(L"\\/"); return slash == std::wstring::npos ? s : s.substr(slash + 1); }
inline std::wstring StemOf(const std::wstring& name) { const auto dot = name.find_last_of(L'.'); return dot == std::wstring::npos ? name : name.substr(0, dot); }
// "song.mp3", "C:\Music\song.flac": the player published a file name, not a title.
inline bool LooksLikeFile(const std::wstring& title) { return AudioExtension(ExtensionOf(FileNameOf(title))); }

// "01 - Artist - Title", "01. Title", "Artist - Title": title and artist from a file name (no extension).
inline void ParseFileName(std::wstring stem, std::wstring& title, std::wstring& artist) {
    stem = TrimText(stem);
    size_t i = 0; while (i < stem.size() && i < 3 && std::iswdigit(stem[i])) ++i;  // leading track number
    if (i > 0 && i < stem.size() && (stem[i] == L'.' || stem[i] == L' ' || stem[i] == L'-' || stem[i] == L'_')) {
        size_t j = i; while (j < stem.size() && (stem[j] == L'.' || stem[j] == L' ' || stem[j] == L'-' || stem[j] == L'_')) ++j;
        if (j < stem.size()) stem = stem.substr(j);
    }
    for (const wchar_t* sep : {L" - ", L" – ", L" — "}) {
        const auto at = stem.find(sep);
        if (at != std::wstring::npos && at > 0 && at + wcslen(sep) < stem.size()) {
            artist = TrimText(stem.substr(0, at)); title = TrimText(stem.substr(at + wcslen(sep)));
            if (!artist.empty() && !title.empty()) return;
        }
    }
    title = stem; artist.clear();
}

class Resolver {
public:
    // Runs on the media worker thread (COM initialized there). Cached per published title/artist/album.
    Result Resolve(const std::wstring& title, const std::wstring& artist, const std::wstring& album) {
        const std::wstring key = title + L"\x1f" + artist + L"\x1f" + album;
        if (key == lastKey_) return last_;
        lastKey_ = key; last_ = Result{};
        const bool fileName = LooksLikeFile(title);
        if (!fileName && !artist.empty()) return last_;  // a real title and artist: nothing to do
        const std::wstring name = FileNameOf(TrimText(title));
        std::wstring path;
        if (fileName && name != title && GetFileAttributesW(title.c_str()) != INVALID_FILE_ATTRIBUTES) path = title;  // a full path
        if (path.empty() && fileName) path = FromRecent(name);
        if (path.empty()) path = FromIndex(Lower(name), fileName);
        Result r;
        if (!path.empty()) ReadFile(path, r);
        if (r.title.empty()) {  // not found, or untagged: the file name says it
            if (!fileName && path.empty()) return last_;
            ParseFileName(StemOf(path.empty() ? name : FileNameOf(path)), r.title, r.artist);
        }
        if (r.album.empty()) r.album = album;
        r.changed = true;
        last_ = r;
        return last_;
    }

private:
    static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
        PWSTR p = nullptr; std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
        CoTaskMemFree(p); return out;
    }
    static std::wstring ResolveLink(const std::wstring& lnk) {
        IShellLinkW* link = nullptr; std::wstring out;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return out;
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            wchar_t target[MAX_PATH * 4]{};
            if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH))) out = target;
            file->Release();
        }
        link->Release();
        if (!out.empty() && GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) out.clear();
        return out;
    }
    static std::wstring FromRecent(const std::wstring& name) {
        const std::wstring recent = KnownFolder(FOLDERID_Recent);
        if (recent.empty()) return {};
        return ResolveLink(recent + L"\\" + name + L".lnk");
    }
    std::wstring FromIndex(const std::wstring& lowerName, bool withExtension) {
        auto find = [&]() -> std::wstring {
            const auto& map = withExtension ? byName_ : byStem_;  // "song.mp3" by file name, "song" by name without extension
            const auto it = map.find(lowerName);
            return it == map.end() ? L"" : it->second;
        };
        std::wstring found = indexed_ ? find() : L"";
        if (found.empty() && (!indexed_ || GetTickCount64() - indexedAt_ > 60000)) { BuildIndex(); found = find(); }
        if (!found.empty() && GetFileAttributesW(found.c_str()) == INVALID_FILE_ATTRIBUTES) found.clear();
        return found;
    }
    void BuildIndex() {
        byName_.clear(); byStem_.clear(); indexed_ = true; indexedAt_ = GetTickCount64();
        size_t budget = 60000;  // directory entries visited at most
        for (REFKNOWNFOLDERID id : {FOLDERID_Music, FOLDERID_Downloads, FOLDERID_Desktop, FOLDERID_Documents}) {
            const std::wstring root = KnownFolder(id);
            if (!root.empty()) Walk(root, 0, budget);
        }
    }
    void Walk(const std::wstring& dir, int depth, size_t& budget) {
        WIN32_FIND_DATAW f{};
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> subdirs;
        do {
            if (!budget--) break;
            const std::wstring n = f.cFileName;
            if (n == L"." || n == L"..") continue;
            if (f.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6) subdirs.push_back(dir + L"\\" + n); continue; }
            if (!AudioExtension(ExtensionOf(n))) continue;
            const std::wstring lower = Lower(n), full = dir + L"\\" + n;
            byName_.emplace(lower, full); byStem_.emplace(StemOf(lower), full);
        } while (FindNextFileW(h, &f));
        FindClose(h);
        for (const auto& d : subdirs) { if (!budget) break; Walk(d, depth + 1, budget); }
    }
    static std::wstring StringProp(IPropertyStore* store, REFPROPERTYKEY key) {
        PROPVARIANT v; PropVariantInit(&v); std::wstring out;
        if (SUCCEEDED(store->GetValue(key, &v)) && v.vt != VT_EMPTY) {
            PWSTR text = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(v, &text)) && text) { out = text; CoTaskMemFree(text); }
        }
        PropVariantClear(&v);
        for (auto& c : out) if (c == L';') c = L',';  // several artists come joined with "; "
        return TrimText(out);
    }
    static void ReadFile(const std::wstring& path, Result& r) {
        r.path = path;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
            r.title = StringProp(store, PKEY_Title);
            r.artist = StringProp(store, PKEY_Music_Artist);
            if (r.artist.empty()) r.artist = StringProp(store, PKEY_Music_AlbumArtist);
            r.album = StringProp(store, PKEY_Music_AlbumTitle);
            r.lyricsText = StringProp(store, PKEY_Music_Lyrics);
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &v)) && v.vt == VT_UI8) r.duration = static_cast<int64_t>(v.uhVal.QuadPart);
            PropVariantClear(&v);
            store->Release();
        }
        // Lyrics beside the file win over the tag: "<name>.lrc" (synced) or "<name>.txt".
        const std::wstring stem = StemOf(path);
        for (const wchar_t* ext : {L".lrc", L".LRC", L".txt"}) {
            const std::wstring side = stem + ext;
            if (GetFileAttributesW(side.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            HANDLE h = CreateFileW(side.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER size{}; std::string bytes;
            if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 2 * 1024 * 1024) {
                bytes.resize(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
                if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) read = 0;
                bytes.resize(read);
            }
            CloseHandle(h);
            if (!bytes.empty()) { r.lyricsText.clear(); r.lyricsBytes = std::move(bytes); break; }
        }
        Cover(path, r);
    }
    static void Cover(const std::wstring& path, Result& r) {
        IShellItemImageFactory* factory = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return;
        HBITMAP bmp = nullptr;
        // Thumbnail only: the embedded album picture, never the file type's icon.
        if (SUCCEEDED(factory->GetImage(SIZE{600, 600}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp)) && bmp) {
            BITMAP info{};
            if (GetObjectW(bmp, sizeof(info), &info) && info.bmWidth > 0 && info.bmHeight != 0) {
                const int w = info.bmWidth, h = std::abs(info.bmHeight);
                BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                auto px = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
                HDC dc = GetDC(nullptr);
                const bool ok = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), px->data(), &bi, DIB_RGB_COLORS) == h;
                ReleaseDC(nullptr, dc);
                if (ok) {
                    bool anyAlpha = false;
                    for (size_t i = 3; i < px->size(); i += 4) if ((*px)[i]) { anyAlpha = true; break; }
                    if (!anyAlpha) for (size_t i = 3; i < px->size(); i += 4) (*px)[i] = 255;  // opaque JPEG covers come with alpha 0
                    r.cover = px; r.coverW = static_cast<uint32_t>(w); r.coverH = static_cast<uint32_t>(h);
                }
            }
            DeleteObject(bmp);
        }
        factory->Release();
    }

    std::wstring lastKey_; Result last_;
    std::unordered_map<std::wstring, std::wstring> byName_, byStem_;
    bool indexed_ = false; ULONGLONG indexedAt_ = 0;
};

}  // namespace nowplaying::localtags
