#pragma once
#include <filesystem>
#include <cstdio>
#include <vector>
#include <string>

namespace docky::pairing {
struct LanPeer { std::wstring id, name; };

class LanPicker {
    HANDLE scan_ = nullptr;
    std::wstring output_;
    double next_ = 0;
    bool failed_ = false;
    std::filesystem::path executable_, helper_;
    HANDLE Start(std::wstring args, bool console = false) const {
        std::wstring command = L"\"" + helper_.wstring() + L"\" " + args;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(helper_.c_str(), command.data(), nullptr, nullptr, FALSE,
                console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    void EndScan() {
        if (scan_) CloseHandle(scan_);
        scan_ = nullptr;
        if (!output_.empty()) DeleteFileW(output_.c_str());
        output_.clear();
    }
public:
    std::vector<LanPeer> peers;
    LanPicker() {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr,path,32768);
        executable_ = path;
        helper_ = executable_.parent_path() / L"dockdrop-pc" / L"DockDropPc.exe";
    }
    ~LanPicker() {
        if (scan_) WaitForSingleObject(scan_, 3500);
        EndScan();
    }
    bool Failed() const { return failed_; }
    bool Poll(double now) {
        if (scan_) {
            if (WaitForSingleObject(scan_,0) != WAIT_OBJECT_0) return false;
            DWORD status = 1;
            GetExitCodeProcess(scan_, &status);
            FILE* input = nullptr;
            if (status == 0) _wfopen_s(&input,output_.c_str(),L"rt, ccs=UTF-16LE");
            std::vector<LanPeer> found;
            if (input) {
                wchar_t line[1024];
                while (fgetws(line,1024,input)) {
                    std::wstring row(line);
                    if (row.size()<34 || row[32]!=L'\t' ||
                        row.substr(0,32).find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos) continue;
                    row.resize(row.find_last_not_of(L"\r\n")+1);
                    found.push_back({row.substr(0,32),row.substr(33)});
                }
                fclose(input);
            }
            failed_ = input == nullptr;
            peers = std::move(found);
            EndScan();
            next_ = now + 2;
            return true;
        }
        if (now < next_) return false;
        next_ = now + 5;
        wchar_t dir[MAX_PATH]{}, file[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH,dir) && GetTempFileNameW(dir,L"ddp",0,file)) {
            output_ = file;
            scan_ = Start(L"picker-list \"" + output_ + L"\"");
        }
        if (!scan_) { failed_ = true; peers.clear(); EndScan(); return true; }
        return false;
    }
    bool Receive() const {
        HANDLE proc = Start(L"picker-receive \"" + executable_.wstring() + L"\"", true);
        if (!proc) return false;
        CloseHandle(proc);
        return true;
    }
    bool Send(const std::wstring& id) const {
        HANDLE proc = Start(L"picker-send " + id + L" \"" + executable_.wstring() + L"\"");
        if (!proc) return false;
        CloseHandle(proc);
        return true;
    }
};
}
