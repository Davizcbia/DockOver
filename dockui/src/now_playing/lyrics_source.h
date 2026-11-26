#pragma once
// Lyrics for the playing track, on a worker thread (latest request wins):
//  1. The user's own files in %LOCALAPPDATA%\LiquidDesktop\lyrics: "Artist - Title.lrc", "Title - Artist.lrc" or
//     "Title.lrc" (.txt for plain text); UTF-8, UTF-16 or GBK. Always preferred, never cached.
//  2. The disk cache of earlier lookups (artwork\lyrics\<key>.lrc, .none markers retried after 3 days).
//  3. NetEase Cloud Music, QQ Music and Kugou public web APIs (no account): synced, broad coverage incl. Chinese.
//     Original lyrics only for now (translations are dropped); title/credit lines at the start are removed.
//  4. LRCLIB (lrclib.net, open lyrics database, no account): by artist, title, album and length; then its search.
//     Every online result must have the same artist, title and version (remix, live, ...) and a length within 3 s.
// Windows media sessions carry no lyrics, so lyrics shown inside a player cannot be read from it.
#include "lyrics.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nowplaying {

// localText: lyrics of the matched local music file (local_tags.h); used first when it parses.
struct LyricsRequest {
    std::wstring title, artist, album;
    int durationMs = 0;
    bool online = true;
    std::wstring tag, localText;
    bool appleCatalog = false;
};
struct LyricsResult { std::wstring tag; lyrics::Lyrics lyrics; };

class LyricsSource {
public:
    using Callback = std::function<void(const LyricsResult&)>;  // worker thread; empty lyrics = none found
    explicit LyricsSource(Callback callback);
    ~LyricsSource();
    LyricsSource(const LyricsSource&) = delete;
    LyricsSource& operator=(const LyricsSource&) = delete;
    void Request(const LyricsRequest& request);
    LyricsResult RunNow(const LyricsRequest& request);  // diagnostics (--lyrics)

    static std::wstring CustomDir();  // created on demand
    // Decodes a lyrics file: UTF-8 (with or without BOM), UTF-16 LE/BE with BOM, else GBK.
    static std::wstring DecodeText(const std::string& bytes);

private:
    void Loop();
    // A preview can show the first synced match while the worker looks for native word timing.
    // A null preview only checks whether the request is still current.
    using Preview = std::function<bool(const lyrics::Lyrics*)>;
    lyrics::Lyrics Find(const LyricsRequest& request, const Preview& preview = {});
    Callback callback_;
    std::mutex mutex_; std::condition_variable wake_;
    std::optional<LyricsRequest> pending_; bool stop_ = false;
    std::thread thread_;
};

}  // namespace nowplaying
