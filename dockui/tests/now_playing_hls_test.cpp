// Parsing for the Now Playing motion artwork fetcher (src/now_playing/hls.h). Fixtures follow the
// shape of Apple Music's public album pages and HLS playlists as observed on 2026-11-20.
#include "../src/now_playing/artwork_match.h"
#include "../src/now_playing/artwork_source.h"
#include "../src/now_playing/audio_beat.h"
#include "../src/now_playing/hls.h"
#include "../src/now_playing/lyrics.h"

#include <algorithm>
#include <vector>

#include <cstdio>

using namespace nowplaying::hls;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // Recording identity must survive search fallbacks and never share a stale album cache.
    {
        using namespace nowplaying;
        CHECK(SameRecording(L"Signal (Nova Remix)", L"Signal - Nova RMX"));
        CHECK(SameRecording(L"Signal（DJ混音）", L"Signal【DJ混音】"));
        CHECK(!SameRecording(L"Signal", L"Signal (Nova Remix)"));
        CHECK(!SameRecording(L"Signal (Nova Remix)", L"Signal (Mira Remix)"));
        CHECK(!SameRecording(L"Signal", L"Signal Again"));

        ArtworkRequest original{L"Signal", L"Artist", L"Signals"};
        original.durationMs = 180000;
        const auto score = [&](const wchar_t* title, const wchar_t* album, bool albumOnly = false, int ms = 180000) {
            return match::CatalogScore(original, L"Artist", title, album, albumOnly, false, ms);
        };
        CHECK(score(L"Signal", L"Signals") > 0);
        CHECK(score(L"Signal (Nova Remix)", L"Signals") == 0);
        CHECK(score(L"Signal", L"Signals (Remixes)") == 0);
        CHECK(score(L"Signal", L"Signals", false, 188000) == 0);
        CHECK(score(L"Signals - Single", L"", true) > 0);
        CHECK(score(L"Signals (Remixes)", L"", true) == 0);
        CHECK(score(L"Signals Deluxe", L"", true) == 0);

        ArtworkRequest remix = original;
        remix.title = L"Signal (Nova Remix)";
        CHECK(match::CatalogScore(remix, L"Artist", L"Signal - Nova RMX", L"Signals") > 0);
        CHECK(match::CatalogScore(remix, L"Artist", L"Signal (Mira Remix)", L"Signals") == 0);
        CHECK(match::CatalogScore(remix, L"Artist", L"Signals", L"", true) == 0);
        CHECK(ArtworkSource::KeyFor(original) != ArtworkSource::KeyFor(remix));
        remix.title = L"Signal (Mira Remix)";
        CHECK(ArtworkSource::KeyFor(original) != ArtworkSource::KeyFor(remix));
        auto noAlbum = original;
        noAlbum.album.clear();
        CHECK(match::CatalogScore(noAlbum, L"Artist", L"Signal", L"Signals (Remixes)") == 0);
    }
    // Resolve
    CHECK(Resolve("https://a.b/x/y/list.m3u8", "seg.mp4") == "https://a.b/x/y/seg.mp4");
    CHECK(Resolve("https://a.b/x/y/list.m3u8?t=1", "seg.mp4") == "https://a.b/x/y/seg.mp4");
    CHECK(Resolve("https://a.b/x/y/list.m3u8", "/z.mp4") == "https://a.b/z.mp4");
    CHECK(Resolve("https://a.b/x/list.m3u8", "https://c.d/e.mp4") == "https://c.d/e.mp4");
    CHECK(Resolve("https://a.b/x/list.m3u8", "//c.d/e.mp4") == "https://c.d/e.mp4");

    // Master playlist: I-frame entries ignored, quoted CODECS with commas, H.264 preferred over HEVC.
    const std::string master =
        "#EXTM3U\n#EXT-X-VERSION:7\n"
        "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=170549,CODECS=\"avc1.64001f\",RESOLUTION=486x486,URI=\"https://h/i.m3u8\"\n"
        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=2151458,BANDWIDTH=2487185,CODECS=\"avc1.64001f\",FRAME-RATE=24.000,RESOLUTION=768x768\n"
        "https://h/v768.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1401104,CODECS=\"hvc1.2.20000000.L123.B0\",RESOLUTION=2160x2160\n"
        "https://h/hevc2160.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4228721,CODECS=\"avc1.640020\",RESOLUTION=1080x1080\n"
        "a/v1080a.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=6587860,CODECS=\"avc1.640020,mp4a.40.2\",RESOLUTION=1080x1080\n"
        "a/v1080b.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=9000000,CODECS=\"avc1.640033\",RESOLUTION=3840x3840\n"
        "a/v4k.m3u8\n";
    const auto variants = ParseMaster(master, "https://h/root/master.m3u8");
    CHECK(variants.size() == 5);
    CHECK(variants[0].width == 768 && variants[0].height == 768 && variants[0].bandwidth == 2487185);
    CHECK(variants[3].codecs == "avc1.640020,mp4a.40.2");
    const Variant* pick = PickVariant(variants, 2160);
    CHECK(pick && pick->uri == "https://h/root/a/v1080b.m3u8");
    // Only HEVC available: it is used.
    const auto hevcOnly = ParseMaster("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"hvc1.2\",RESOLUTION=720x720\nh.m3u8\n", "https://h/m.m3u8");
    CHECK(PickVariant(hevcOnly, 2160) && PickVariant(hevcOnly, 2160)->uri == "https://h/h.m3u8");
    CHECK(PickVariant({}, 2160) == nullptr);

    // Media playlist: one fMP4 file addressed by byte ranges (Apple's motion artwork form).
    const std::string media =
        "#EXTM3U\n#EXT-X-TARGETDURATION:3\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-MAP:URI=\"v-.mp4\",BYTERANGE=\"899@0\"\n"
        "#EXTINF:2.83333,\t\n#EXT-X-BYTERANGE:2028157@899\nv-.mp4\n"
        "#EXTINF:2.83333,\n#EXT-X-BYTERANGE:2044736@2029056\nv-.mp4\n"
        "#EXTINF:2.875,\n#EXT-X-BYTERANGE:100\nv-.mp4\n"
        "#EXT-X-ENDLIST\n";
    const auto ranges = ParseMedia(media, "https://h/x/v.m3u8");
    CHECK(ranges.size() == 4);
    CHECK(ranges[0].uri == "https://h/x/v-.mp4" && ranges[0].offset == 0 && ranges[0].length == 899);
    CHECK(ranges[1].offset == 899 && ranges[1].length == 2028157);
    CHECK(ranges[3].offset == 2029056 + 2044736 && ranges[3].length == 100);  // offset continues
    const auto merged = Coalesce(ranges);
    CHECK(merged.size() == 1 && merged[0].offset == 0 && merged[0].length == 899 + 2028157 + 2044736 + 100);
    // Separate segment files stay separate requests.
    const auto plain = Coalesce(ParseMedia("#EXTM3U\n#EXTINF:2,\na.mp4\n#EXTINF:2,\nb.mp4\n", "https://h/v.m3u8"));
    CHECK(plain.size() == 2 && !plain[0].ranged && plain[1].uri == "https://h/b.mp4");
    // Encrypted or not a playlist: rejected.
    CHECK(ParseMedia("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:2,\na.mp4\n", "https://h/v.m3u8").empty());
    CHECK(ParseMedia("<html>", "https://h/v.m3u8").empty());

    // Album page scanning.
    const std::string page =
        "...\"shuffleButton\":null,\"videoArtwork\":{\"dictionary\":{\"motionDetailSquare\":{\"previewFrame\":{\"bgColor\":\"0e0a40\","
        "\"url\":\"https://is1/x.jpg\"},\"video\":\"https:\\u002F\\u002Fmvod.itunes.apple.com\\u002Fsq_default.m3u8\"},"
        "\"motionDetailTall\":{\"previewFrame\":{\"url\":\"https://is1/t.jpg\"},\"video\":\"https://mvod.itunes.apple.com/tall_default.m3u8\"}}},...";
    CHECK(FindMotionVideo(page, false) == "https://mvod.itunes.apple.com/sq_default.m3u8");
    CHECK(FindMotionVideo(page, true) == "https://mvod.itunes.apple.com/tall_default.m3u8");
    CHECK(FindMotionVideo("\"videoArtwork\":null,\"tallVideoArtwork\":null", false).empty());
    CHECK(FindMotionVideo("\"videoArtwork\":{\"dictionary\":{\"motionDetailSquare\":{\"video\":\"javascript:x\"}}}", false).empty());

    CHECK(Attribute("A=1,B=\"x,y\",C=z", "B") == "x,y");
    CHECK(Attribute("A=1,B=\"x,y\",C=z", "C") == "z");
    CHECK(Attribute("A=1", "Q").empty());

    // tfdt rebasing: moof/traf/tfdt v0 240 and v1 308 -> 0 and 68; non-MP4 untouched.
    {
        auto box = [](const char* type, const std::string& body) {
            const uint32_t n = uint32_t(body.size() + 8); std::string b;
            for (int i = 3; i >= 0; --i) b += char((n >> (8 * i)) & 0xff);
            return b + type + body;
        };
        auto be = [](uint64_t v, int bytes) { std::string s; for (int i = bytes - 1; i >= 0; --i) s += char((v >> (8 * i)) & 0xff); return s; };
        std::string file = box("ftyp", "isom") + box("moov", box("mvhd", std::string(12, char(0)))) +
            box("moof", box("mfhd", be(0, 8)) + box("traf", box("tfhd", be(0, 8)) + box("tfdt", be(0, 4) + be(240, 4)))) + box("mdat", "xx") +
            box("moof", box("traf", box("tfdt", be(1u << 24, 4) + be(308, 8)))) + box("mdat", "yy");
        CHECK(RebaseFragments(file));
        CHECK(file.find(be(0, 4) + be(0, 4)) != std::string::npos);
        CHECK(file.find(be(1u << 24, 4) + be(68, 8)) != std::string::npos);
        std::string junk = "not an mp4 at all";
        CHECK(!RebaseFragments(junk) && junk == "not an mp4 at all");
    }

    // Cover check: the same picture at another size matches; another picture does not; a 16:9 frame with the
    // cover centred matches the square cover.
    {
        auto picture = [](uint32_t w, uint32_t h, int variant, uint32_t pad) {
            std::vector<uint8_t> px(size_t(w) * h * 4, 0);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* p = &px[(size_t(y) * w + x) * 4];
                    if (x < pad || x >= w - pad) continue;  // black bars
                    const float u = float(x - pad) / float(w - 2 * pad), v = float(y) / float(h);
                    const float value = variant == 0 ? (u * 0.7f + (v > 0.5f ? 0.3f : 0.f)) * 255 : ((u > 0.3f && u < 0.6f) ? 220.f : 40.f) + v * 30;
                    p[0] = p[1] = p[2] = uint8_t(value); p[3] = 255;
                }
            return px;
        };
        using namespace nowplaying::match;
        const auto a = picture(600, 600, 0, 0), b = picture(100, 100, 0, 0), c = picture(100, 100, 1, 0), d = picture(1600, 900, 0, 350);
        const uint64_t ha = DHash(a.data(), 600, 600, 600 * 4), hb = DHash(b.data(), 100, 100, 100 * 4), hc = DHash(c.data(), 100, 100, 100 * 4);
        const uint64_t hd = DHash(d.data(), 1600, 900, 1600 * 4);
        CHECK(Distance(ha, hb) <= kSameCover);
        CHECK(Distance(ha, hc) > kSameCover);
        CHECK(Distance(ha, hd) <= kSameCover);
        CHECK(DHash(nullptr, 0, 0, 0) == 0);
    }

    // Apple Music web search page items (escaped and literal UTF-8 separators); a playlist item is skipped.
    {
        const std::string page =
            R"({"id":"top - 1","headline":null,"subtitle":"Album · Excision & Subtronics","itemKind":"albums","subheadline":null,)"
            R"("artwork":{"dictionary":{"bgColor":"1e1b38","url":"https://is1-ssl.mzstatic.com/image/thumb/x/810060817407.png/{w}x{h}bb.{f}","width":1600}},)"
            R"("title":"A.F.B.1. - Single","contentDescriptor":{"kind":"album","identifiers":{"storeAdamID":"1874888958"},"url":"https://music.apple.com/us/album/a-f-b-1-single/1874888958"}},)"
            "{\"subtitle\":\"Song\xE2\x80\xAF\xC2\xB7\xE2\x80\xAF\xE5\x91\xA8\xE6\x9D\xB0\xE4\xBC\xA6\",\"itemKind\":\"songs\",\"artwork\":{\"dictionary\":{\"url\":\"https://is1/a/{w}x{h}bb.{f}\"}},"
            "\"title\":\"\\u6674\\u5929\",\"contentDescriptor\":{\"kind\":\"song\",\"url\":\"https://music.apple.com/cn/album/x/1?i=2\"}},"
            R"({"subtitle":"Playlist · Apple Music","itemKind":"playlists","artwork":{"dictionary":{"url":"https://is1/p/{w}x{h}bb.{f}"}},"title":"Hits","contentDescriptor":{"kind":"playlist","url":"https://music.apple.com/us/playlist/p"}})";
        const auto items = ParseWebSearch(page);
        CHECK(items.size() == 2);
        if (items.size() == 2) {
            CHECK(items[0].kind == "albums" && items[0].title == "A.F.B.1. - Single" && items[0].artist == "Excision & Subtronics");
            CHECK(items[0].url == "https://music.apple.com/us/album/a-f-b-1-single/1874888958");
            CHECK(items[0].artwork == "https://is1-ssl.mzstatic.com/image/thumb/x/810060817407.png/{w}x{h}bb.{f}");
            CHECK(items[1].kind == "songs" && items[1].artist == "\xE5\x91\xA8\xE6\x9D\xB0\xE4\xBC\xA6" && items[1].title == "\xE6\x99\xB4\xE5\xA4\xA9");
        }
        CHECK(UnescapeJson("\\u00e9\\ud83c\\udfb5") == "\xC3\xA9\xF0\x9F\x8E\xB5");
    }

    // LRC parsing.
    {
        using namespace nowplaying::lyrics;
        CHECK(ParseTime(L"01:02.50") == 62500 && ParseTime(L"1:02") == 62000 && ParseTime(L"00:05.123") == 5123 && ParseTime(L"00:05:12") == 5120);
        CHECK(ParseTime(L"ti:Song") == -1 && ParseTime(L"00:75") == -1);
        const auto l = ParseLrc(L"\xFEFF[ti:Song]\r\n[ar:Someone]\r\n[offset:+500]\r\n[00:12.00][00:40.00]Chorus\r\n[00:05.00]Hello <00:05.20>world\r\n"
                                L"[00:05.00]\u4f60\u597d\u4e16\u754c\r\n[00:20.00]\r\n[00:21.00]\r\n[00:30.00]Bridge\r\n[00:50.00]\r\n");
        CHECK(l.synced);
        CHECK(l.lines.size() == 5);
        if (l.lines.size() == 5) {
            CHECK(l.lines[0].ms == 4500 && l.lines[0].text == L"Hello world" && l.lines[0].translation == L"\u4f60\u597d\u4e16\u754c");
            CHECK(l.lines[1].ms == 11500 && l.lines[1].text == L"Chorus");
            CHECK(l.lines[2].text.empty() && l.lines[2].ms == 19500);  // one gap kept, the repeated one dropped
            CHECK(l.lines[3].text == L"Bridge" && l.lines[4].text == L"Chorus" && l.lines[4].ms == 39500);  // trailing gap dropped
        }
        CHECK(l.LineAt(0) == -1 && l.LineAt(4500) == 0 && l.LineAt(30000) == 3 && l.LineAt(100000) == 4);
        // NetEase credits at the start are dropped; a lyric with a colon is kept; translations merge by time.
        auto credited = ParseLrc(L"[00:00.000] 作词 : Someone\n[00:01.000] Producer : Name\n[00:12.00]I said: hello\n[00:15.00]Line two\n");
        DropCredits(credited);
        CHECK(credited.lines.size() == 2 && credited.lines[0].text == L"I said: hello");
        auto english = ParseLrc(L"[00:12.00]Hello\n[00:15.00]World\n");
        MergeTranslation(english, ParseLrc(L"[00:12.010]你好\n[00:15.00]世界\n"));
        CHECK(english.lines[0].translation == L"你好" && english.lines[1].translation == L"世界");
        const auto plain = ParseLrc(L"First line\n\nSecond line\n");
        CHECK(!plain.synced && plain.lines.size() == 2 && plain.LineAt(5000) == -1);
    }

    // Palette: a red/blue image gives red and blue first; a flat image gives one colour.
    {
        std::vector<uint8_t> px(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) { uint8_t* p = &px[(size_t(y) * 64 + x) * 4]; if (x < 40) { p[2] = 220; p[1] = 30; p[0] = 30; } else { p[2] = 20; p[1] = 40; p[0] = 200; } p[3] = 255; }
        const auto pal = nowplaying::match::Palette(px.data(), 64, 64, 64 * 4, 4);
        CHECK(!pal.empty() && (pal[0] >> 16) > 200 && (pal[0] & 0xFF) < 60);              // red, the larger area, first
        CHECK(std::any_of(pal.begin(), pal.end(), [](uint32_t c) { return (c & 0xFF) > 180 && (c >> 16) < 60; }));  // blue present
        std::vector<uint8_t> flat(16 * 16 * 4, 128);
        const auto one = nowplaying::match::Palette(flat.data(), 16, 16, 16 * 4, 4);
        CHECK(one.size() == 1);
    }

    // Beat tracker: steady energy and silence give no pulses; a 120 BPM kick train locks to ~120 BPM, pulses land on
    // the kicks (up to the 30 ms lead), and a few missing kicks are still pulsed from the grid.
    {
        nowplaying::BeatTracker steady; int beats = 0;
        for (int i = 0; i < 500; ++i) if (steady.Feed(.01f, .002f, .01f) > 0) ++beats;
        CHECK(beats == 0);
        nowplaying::BeatTracker quiet; int q = 0;
        for (int i = 0; i < 300; ++i) if (quiet.Feed(0.f, 0.f, .01f) > 0) ++q;
        CHECK(q == 0);

        nowplaying::BeatTracker d; unsigned seed = 7;
        auto noise = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
        int late = 0, onGrid = 0, filled = 0;
        for (int i = 0; i < 2000; ++i) {  // 20 s, kick every 0.5 s (50 blocks), kicks 24..27 missing
            const int phase = i % 50, kick = i / 50;
            const bool missing = kick >= 24 && kick <= 27;
            const float k = !missing && phase < 6 ? .08f * std::exp(-phase * .5f) : 0.f;
            const float s = d.Feed(.004f + .002f * noise() + k, .003f + .002f * noise() + (phase == 0 && !missing ? .01f : 0.f), .01f);
            if (s <= 0 || i < 800) continue;
            // distance to the nearest kick in seconds (pulse may lead by up to ~40 ms)
            const float off = float(phase < 25 ? phase : phase - 50) * .01f;
            if (off >= -.05f && off <= .03f) { ++onGrid; if (missing) ++filled; } else ++late;
        }
        CHECK(d.Locked() && std::fabs(d.Bpm() - 120.f) < 4.f);
        CHECK(onGrid >= 20 && late <= 2);
        CHECK(filled >= 2);
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("now_playing_hls_test: all passed\n");
    return 0;
}
