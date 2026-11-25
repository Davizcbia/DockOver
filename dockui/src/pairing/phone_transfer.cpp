#include "phone_transfer.h"
#include "../json_lite.h"
#include "phone_language.h"
using docky::pairing::Ui;
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <array>
#include <cwctype>

namespace docky::pairing {
namespace {
constexpr uint64_t kMaxFile = 4ull * 1024 * 1024 * 1024;
constexpr DWORD kChunk = 32 * 1024;
constexpr uint64_t kPhoneBatch = 128ull * 1024 * 1024;
constexpr size_t kPhoneFileCount = 64;
using Microsoft::WRL::ComPtr;

std::wstring Join(const std::wstring& dir, std::wstring_view leaf) {
    return dir + (dir.ends_with(L"\\") ? L"" : L"\\") + std::wstring(leaf);
}

std::wstring SafeName(std::wstring_view input) {
    if (input.empty() || input.size() > 255) return {};
    std::wstring name;
    for (wchar_t ch : input) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == 0) return {};
        name += ch < 32 || std::wstring_view(L"<>\"|?*").find(ch) != std::wstring_view::npos ? L'_' : ch;
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty() || name == L"." || name == L"..") return {};
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    for (auto& c : base) c = static_cast<wchar_t>(std::towupper(c));
    const bool device = base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL"
        || base == L"CONIN$" || base == L"CONOUT$"
        || (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))
            && std::wstring_view(L"0123456789¹²³").find(base.back()) != std::wstring_view::npos);
    if (device) name.insert(name.begin(), L'_');
    return name;
}

bool SetRegistryString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(), name, REG_SZ, value.c_str(), size) == ERROR_SUCCESS;
}
}  // namespace

std::wstring DownloadFolder() {
    LPWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path))
        && FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &path))) return {};
    std::wstring folder(path);
    CoTaskMemFree(path);
    return folder;
}

std::wstring ChooseReceiveFolder(HWND owner, const std::wstring& folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags{};
    if (FAILED(dialog->GetOptions(&flags)) || FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM))) return {};
    dialog->SetTitle(Ui(L"选择 DockDrop 接收文件夹"));
    ComPtr<IShellItem> initial;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial.Get());
    }
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return {};
    PWSTR path = nullptr;
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

bool RegisterDockDropMenu(const wchar_t* executable) {
    if (!executable || !*executable || std::wstring_view(executable).find(L'"') != std::wstring_view::npos) return false;
    const std::wstring command = L"\"" + std::wstring(executable) + L"\" --pair-phone";
    bool success = true;
    for (const auto kind : {L"DesktopBackground", L"Directory\\Background", L"Directory", L"*"}) {
        const auto key = L"Software\\Classes\\" + std::wstring(kind) + L"\\shell\\DockOver.DockDropReceive";
        const auto invocation = std::wstring_view(kind) == L"*" ? command + L" --send-file \"%1\"" : command;
        const bool registered = SetRegistryString(key + L"\\command", nullptr, invocation)
            && SetRegistryString(key, L"MUIVerb", L"DockDrop")
            && SetRegistryString(key, L"Icon", executable);
        success = registered && success;
    }
    if (success) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return success;
}

std::vector<std::wstring> ChooseSendFiles(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags{};
    if (FAILED(dialog->GetOptions(&flags)) ||
        FAILED(dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST))) return {};
    dialog->SetTitle(Ui(L"选择要发送到手机的文件"));
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItemArray> selected;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(&selected)) || FAILED(selected->GetCount(&count))) return {};
    std::vector<std::wstring> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(selected->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
        paths.emplace_back(path);
        CoTaskMemFree(path);
    }
    return paths;
}

std::wstring ReadClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    std::wstring text;
    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* value = static_cast<const wchar_t*>(GlobalLock(data))) {
            const size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            const size_t length = wcsnlen(value, std::min<size_t>(capacity, 65537));
            if (length < capacity && length <= 65536) text.assign(value, length);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

bool OutgoingTransfer::SetFiles(const std::vector<std::wstring>& paths) {
    if (paths.size() > kPhoneFileCount) return false;
    OutgoingTransfer next;
    uint64_t total = 0;
    for (const auto& path : paths) {
        const auto name = SafeName(path.substr(path.find_last_of(L"\\/") + 1));
        if (name.empty()) return false;
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size)
            || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kPhoneBatch - total) {
            CloseHandle(file);
            return false;
        }
        total += static_cast<uint64_t>(size.QuadPart);
        next.files_.push_back({file, name, static_cast<uint64_t>(size.QuadPart)});
    }
    files_.swap(next.files_);
    return true;
}

bool OutgoingTransfer::SetText(std::wstring text) {
    if (dock::json::ToUtf8(text).size() > 65536) return false;
    text_ = std::move(text);
    return true;
}

std::wstring OutgoingTransfer::Manifest() const {
    using dock::json::Value;
    auto manifest = Value::MakeObject();
    Value files;
    files.type = Value::Type::Array;
    for (const auto& file : files_) {
        auto offer = Value::MakeObject();
        offer.Set("name", Value::MakeString(dock::json::ToUtf8(file.name)));
        offer.Set("size", Value::MakeNumber(static_cast<double>(file.size)));
        files.array.push_back(std::move(offer));
    }
    manifest.Set("files", std::move(files));
    manifest.Set("text", Value::MakeString(dock::json::ToUtf8(text_)));
    return dock::json::FromUtf8(dock::json::Write(manifest, 0));
}

std::wstring OutgoingTransfer::Read(std::wstring_view range) const {
    auto parse = [](std::wstring_view digits, uint64_t& result) {
        if (digits.empty() || digits.size() > 9) return false;
        for (wchar_t digit : digits) {
            if (digit < L'0' || digit > L'9') return false;
            result = result * 10 + static_cast<uint64_t>(digit - L'0');
        }
        return true;
    };
    const auto divider = range.find(L':');
    uint64_t index = 0, offset = 0;
    if (divider == std::wstring_view::npos || !parse(range.substr(0, divider), index)
        || !parse(range.substr(divider + 1), offset) || index >= files_.size()) return {};
    const auto& file = files_[static_cast<size_t>(index)];
    if (offset >= file.size || offset % kChunk) return {};
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    std::array<BYTE, kChunk> bytes{};
    const auto length = static_cast<DWORD>(std::min<uint64_t>(kChunk, file.size - offset));
    DWORD read = 0, chars = 0;
    constexpr DWORD encoding = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!SetFilePointerEx(file.handle, position, nullptr, FILE_BEGIN)
        || !ReadFile(file.handle, bytes.data(), length, &read, nullptr) || read != length
        || !CryptBinaryToStringW(bytes.data(), read, encoding, nullptr, &chars)) return {};
    std::wstring result(chars, L'\0');
    if (!CryptBinaryToStringW(bytes.data(), read, encoding, result.data(), &chars)) return {};
    result.resize(chars);
    return result;
}

void OutgoingTransfer::Clear() {
    for (const auto& file : files_) CloseHandle(file.handle);
    files_.clear();
    text_.clear();
}

bool IncomingFile::Begin(const std::wstring& folder, std::wstring_view offer) {
    if (file_ != INVALID_HANDLE_VALUE || folder.empty()) return false;
    const auto separator = offer.find(L':');
    if (separator == std::wstring_view::npos || separator == 0 || separator > 10) return false;
    uint64_t bytes = 0;
    for (wchar_t digit : offer.substr(0, separator)) {
        if (digit < L'0' || digit > L'9') return false;
        bytes = 10 * bytes + static_cast<uint64_t>(digit - L'0');
    }
    const auto safe = SafeName(offer.substr(separator + 1));
    if (bytes > kMaxFile || safe.empty()) return false;
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    GUID id{};
    wchar_t guid[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, guid, ARRAYSIZE(guid))) return false;
    const auto temp = Join(folder, L".dockdrop-" + std::wstring(guid) + L".part");
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    file_ = file;
    folder_ = folder;
    temp_ = temp;
    name_ = safe;
    expected_ = bytes;
    received_ = 0;
    return true;
}

bool IncomingFile::Append(std::wstring_view base64) {
    if (file_ == INVALID_HANDLE_VALUE || base64.empty() || base64.size() > 4 * ((kChunk + 2) / 3)) return false;
    std::array<BYTE, kChunk> decoded;
    DWORD count = static_cast<DWORD>(decoded.size()), wrote = 0;
    if (!CryptStringToBinaryW(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
            decoded.data(), &count, nullptr, nullptr) || count == 0 || count > expected_ - received_) return false;
    if (!WriteFile(file_, decoded.data(), count, &wrote, nullptr) || wrote != count) return false;
    received_ += wrote;
    return true;
}

bool IncomingFile::Commit() {
    if (file_ == INVALID_HANDLE_VALUE || received_ != expected_ || !FlushFileBuffers(file_)) return false;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    const auto split = name_.find_last_of(L'.');
    const auto stem = split != 0 ? name_.substr(0, split) : name_;
    const auto ext = split != std::wstring::npos && split != 0 ? name_.substr(split) : L"";
    for (int suffix = 0; suffix < 1000; ++suffix) {
        const auto candidate = suffix ? stem + L" (" + std::to_wstring(suffix) + L")" + ext : name_;
        if (MoveFileExW(temp_.c_str(), Join(folder_, candidate).c_str(), MOVEFILE_WRITE_THROUGH)) {
            temp_.clear();
            name_ = candidate;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

void IncomingFile::Cancel() {
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    if (!temp_.empty()) { DeleteFileW(temp_.c_str()); temp_.clear(); }
    expected_ = received_ = 0;
}

}  // namespace docky::pairing
