#pragma once
// Finds high-resolution and motion album artwork for the current track and caches it on disk.
//  1. iTunes Search API (public, no account): album match, 2000x2000 still.
//  2. The album's public Apple Music page: motion artwork HLS (motionDetailSquare / motionDetailTall).
//     The H.264 variant is downloaded once into a plain fMP4 so playback is local and loops cheaply.
// Cache: %LOCALAPPDATA%\LiquidDesktop\artwork\<key>.jpg / .mp4 / .page / .novideo
// Network use is limited to the artist, album and title of the playing track. It can be turned off
// (online=0 in now-playing.ini); the window then uses the Windows media session thumbnail only.
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nowplaying {

struct ArtworkRequest {
    std::wstring title, artist, album; bool preferTall = false;
    int durationMs = 0;  // track length from the player (0: unknown); separates edits, live and extended versions
    // The player's own cover (BGRA, tightly packed), used to confirm a catalog match. Optional.
    std::shared_ptr<const std::vector<uint8_t>> thumb; uint32_t thumbW = 0, thumbH = 0;
    bool animatedArtwork = true;
};
struct ArtworkResult { std::wstring key, imagePath, videoPath; };

class ArtworkSource {
public:
    // Called on the worker thread, once per finding (still first, then motion when there is one).
    using Callback = std::function<void(const ArtworkResult&)>;
    explicit ArtworkSource(Callback callback);
    ~ArtworkSource();
    ArtworkSource(const ArtworkSource&) = delete;
    ArtworkSource& operator=(const ArtworkSource&) = delete;

    void Request(const ArtworkRequest& request);  // the newest request replaces any pending or running one
    // Runs one request on the calling thread (diagnostics: --fetch).
    void RunNow(const ArtworkRequest& request) { { std::lock_guard lock(mutex_); activeKey_ = RequestKey(request); } Process(request); }

    static std::wstring CacheDir();                     // created on demand
    static std::wstring KeyFor(const ArtworkRequest& r); // stable file stem; empty when nothing to match on

private:
    static std::wstring RequestKey(const ArtworkRequest& request);
    void Loop();
    void Process(const ArtworkRequest& request);
    bool Cancelled(const std::wstring& key);

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ArtworkRequest> pending_;
    std::wstring activeKey_;
    bool stop_ = false;
    std::thread thread_;
};

// HTTPS GET. rangeLength>0 requests bytes [offset, offset+length). Fails above maxBytes.
bool HttpGet(const std::string& url, std::string& body, uint64_t rangeOffset = 0, uint64_t rangeLength = 0,
             size_t maxBytes = 48u * 1024 * 1024, const wchar_t* userAgent = nullptr, const wchar_t* headers = nullptr);

void Log(const wchar_t* format, ...);

// Shared with the lyrics lookup.
ArtworkRequest CleanRequest(const ArtworkRequest& r);   // "Artist — Album" (Apple Music) split
std::wstring NormalizeName(const std::wstring& s);      // comparable form (see artwork_source.cpp)
std::string UrlEncodeQuery(const std::wstring& s);
namespace match {
int CatalogScore(const ArtworkRequest& request, const std::wstring& artist, const std::wstring& name,
                 const std::wstring& collection, bool album = false, bool anyArtist = false, int durationMs = 0);
}
bool SameRecording(const std::wstring& a, const std::wstring& b);
bool SameVersion(const std::wstring& a, const std::wstring& b);  // same remix/live/acoustic/... qualifiers
std::wstring ToSimplified(const std::wstring& s);                // Traditional -> Simplified Chinese (others unchanged)

}  // namespace nowplaying
