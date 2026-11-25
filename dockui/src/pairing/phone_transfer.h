#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace docky::pairing {

std::wstring DownloadFolder();
std::wstring ChooseReceiveFolder(HWND owner, const std::wstring& folder);
bool RegisterDockDropMenu(const wchar_t* executable);
std::vector<std::wstring> ChooseSendFiles(HWND owner);
std::wstring ReadClipboardText(HWND owner);

class OutgoingTransfer {
public:
    ~OutgoingTransfer() { Clear(); }
    bool SetFiles(const std::vector<std::wstring>& paths);
    bool SetText(std::wstring text);
    size_t FileCount() const { return files_.size(); }
    std::wstring Manifest() const;
    std::wstring Read(std::wstring_view range) const;
    void Clear();
private:
    struct File {
        HANDLE handle;
        std::wstring name;
        uint64_t size;
    };
    std::vector<File> files_;
    std::wstring text_;
};

class IncomingFile {
public:
    ~IncomingFile() { Cancel(); }
    bool Begin(const std::wstring& folder, std::wstring_view offer);
    bool Append(std::wstring_view base64);
    bool Commit();
    void Cancel();
    const std::wstring& Name() const { return name_; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring folder_, temp_, name_;
    uint64_t expected_ = 0, received_ = 0;
};

}  // namespace docky::pairing
