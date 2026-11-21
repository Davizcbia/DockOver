#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace docky::music_wallpaper {

// A small premultiplied image of the wallpaper layer. The Desktop process can sample it
// without capturing any foreground windows, widgets, or desktop icons.
inline constexpr unsigned Width = 384, Height = 216;
inline constexpr wchar_t MapName[] = L"Local\\DockOverMusicWallpaperFrameV1";
inline constexpr wchar_t MutexName[] = L"Local\\DockOverMusicWallpaperFrameLockV1";
// Auto-reset event set after every Publish/Clear, so readers repaint at once instead of polling.
inline constexpr wchar_t ReadyEventName[] = L"Local\\DockOverMusicWallpaperFrameReadyV1";
inline HANDLE OpenReadyEvent() { return CreateEventW(nullptr, FALSE, FALSE, ReadyEventName); }

struct Frame {
    std::uint64_t window;
    DWORD owner;
    RECT screen;
    std::uint64_t version;
    std::uint32_t pixels[Width * Height];
};

class Channel {
public:
    Channel() = default;
    ~Channel() { Close(); if (ready_) CloseHandle(ready_); }
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void Publish(HWND window, RECT screen, const BYTE* pixels, unsigned pitch) {
        if (!frame_ && !Open(true)) return;
        if (!Lock()) return;
        for (unsigned y = 0; y < Height; ++y)
            std::memcpy(frame_->pixels + y * Width, pixels + static_cast<size_t>(y) * pitch, Width * 4);
        frame_->screen = screen;
        frame_->window = reinterpret_cast<UINT_PTR>(window);
        frame_->owner = GetCurrentProcessId();
        ++frame_->version;
        ReleaseMutex(mutex_);
        Signal();
    }

    void Clear() {
        if (frame_ && Lock()) {
            frame_->window = 0;
            ReleaseMutex(mutex_);
            Signal();
        }
    }

    bool Read(RECT& screen, std::vector<std::uint32_t>& pixels) {
        if (!frame_ && !Open(false)) return false;
        if (!Lock()) return false;
        const auto window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(frame_->window));
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        const bool valid = pid && pid == frame_->owner && IsWindowVisible(window) &&
            frame_->screen.right > frame_->screen.left && frame_->screen.bottom > frame_->screen.top;
        bool changed = false;
        if (valid && (pixels.empty() || version_ != frame_->version || owner_ != pid)) {
            pixels.assign(frame_->pixels, frame_->pixels + Width * Height);
            screen = frame_->screen;
            version_ = frame_->version;
            owner_ = pid;
            changed = true;
        } else if (!valid && !pixels.empty()) {
            pixels.clear();
            changed = true;
        }
        ReleaseMutex(mutex_);
        if (!valid) Close();
        return changed;
    }

private:
    void Signal() {
        if (!ready_) ready_ = OpenReadyEvent();
        if (ready_) SetEvent(ready_);
    }

    bool Open(bool write) {
        const DWORD access = write ? FILE_MAP_WRITE : FILE_MAP_READ;
        mapping_ = write ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Frame), MapName)
                         : OpenFileMappingW(access, FALSE, MapName);
        if (mapping_) {
            mutex_ = write ? CreateMutexW(nullptr, FALSE, MutexName)
                           : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, MutexName);
            if (mutex_) frame_ = static_cast<Frame*>(MapViewOfFile(mapping_, access, 0, 0, sizeof(Frame)));
        }
        if (frame_) return true;
        Close();
        return false;
    }

    bool Lock() const {
        const DWORD result = WaitForSingleObject(mutex_, 0);
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }

    void Close() {
        if (frame_) UnmapViewOfFile(frame_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
        frame_ = nullptr;
        mapping_ = mutex_ = nullptr;
    }

    HANDLE mapping_ = nullptr, mutex_ = nullptr, ready_ = nullptr;
    Frame* frame_ = nullptr;
    std::uint64_t version_ = 0;
    DWORD owner_ = 0;
};

}  // namespace docky::music_wallpaper
