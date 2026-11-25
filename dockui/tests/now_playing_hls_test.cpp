// Parsing for the Now Playing motion artwork fetcher (src/now_playing/hls.h). Fixtures follow the
// shape of Apple Music's public album pages and HLS playlists as observed on 2026-11-20.
#include "../src/now_playing/artwork_match.h"
#include "../src/now_playing/artwork_source.h"
#include "../src/now_playing/music_analysis.h"
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

    {
        using namespace nowplaying;
        constexpr unsigned rate = 48000;
        for (float volume : {1.f, .1f}) {
            MusicAnalyzer analyzer;
            analyzer.Configure(rate);
            MusicFrame frame;
            unsigned kicks = 0, extraKicks = 0, beats = 0;
            double firstLock = 0;
            for (unsigned i = 0; i < 10 * rate; ++i) {
                const double t = double(i) / rate;
                const double p = std::fmod(t, .5);
                const float bass = .05f * float(std::sin(2 * std::numbers::pi * 73 * t));
                const float kick = p < .085 ? float(.5 * std::exp(-35 * p) * std::sin(2 * std::numbers::pi * 58 * p)) : 0.f;
                if (!analyzer.Push(volume * (bass + kick), t + 1. / rate, frame)) continue;
                if (frame.bpm && !firstLock) firstLock = frame.at;
                for (unsigned e = 0; e < frame.count; ++e) {
                    const auto& event = frame.events[e];
                    if (event.kind == RhythmKind::Kick) {
                        ++kicks;
                        if (std::abs(event.at - std::round(event.at * 2) / 2) > .05) ++extraKicks;
                    }
                    if (event.kind == RhythmKind::Beat) ++beats;
                }
                CHECK(frame.downbeatConfidence < .38f);  // all four accents are alike
            }
            CHECK(kicks >= 18 && extraKicks <= 1);
            CHECK(beats >= 11 && firstLock > 0 && firstLock < 4.);
            CHECK(std::abs(frame.bpm - 120.f) < 2.f);
            for (unsigned i = 0; i < rate; ++i) analyzer.Push(0, 10. + double(i) / rate, frame);
            CHECK(frame.bpm == 0 && frame.level < .01f);
            analyzer.Reset();
            for (unsigned i = 0; i < 2 * rate; ++i) {
                const double t = double(i) / rate;
                const float bass = (t < 1. ? .025f : .16f) * float(std::sin(2 * std::numbers::pi * 73 * t));
                if (!analyzer.Push(bass, t, frame)) continue;
                for (unsigned e = 0; e < frame.count; ++e)
                    CHECK(frame.events[e].kind != RhythmKind::Drop && frame.events[e].kind != RhythmKind::Beat);
            }
        }
        {
            MusicAnalyzer analyzer;
            analyzer.Configure(rate, 0);
            MusicFrame frame;
            unsigned bassKicks = 0;
            for (unsigned i = 0; i < 6 * rate; ++i) {
                const double t = double(i) / rate;
                const float bass = float((.12 + .04 * std::sin(2 * std::numbers::pi * .43 * t)) *
                    std::sin(2 * std::numbers::pi * 61 * t));
                if (!analyzer.Push(bass, t + 1. / rate, frame) || t < .3) continue;
                for (unsigned e = 0; e < frame.count; ++e)
                    bassKicks += frame.events[e].kind == RhythmKind::Kick;
            }
            CHECK(bassKicks == 0);
            analyzer.Reset();
            unsigned firstHits = 0, secondHits = 0;
            for (unsigned i = 0; i < 5 * rate; ++i) {
                const double t = double(i) / rate, p = std::fmod(t, .5);
                const auto note = [](double q) {
                    return q >= 0 && q < .065 ? .55 * std::exp(-46 * q) * std::sin(2 * std::numbers::pi * 58 * q) : 0.;
                };
                if (!analyzer.Push(float(note(p) + note(p - .13)), t + 1. / rate, frame)) continue;
                for (unsigned e = 0; e < frame.count; ++e) {
                    if (frame.events[e].kind != RhythmKind::Kick) continue;
                    const double phase = std::fmod(frame.events[e].at, .5);
                    firstHits += phase < .055;
                    secondHits += phase >= .13 && phase < .185;
                }
            }
            CHECK(firstHits >= 9 && secondHits >= 9);
        }
        {
            MusicAnalyzer analyzer;
            analyzer.Configure(rate, 0);
            MusicFrame frame;
            unsigned onsets = 0, beats = 0;
            for (unsigned i = 0; i < 8 * rate; ++i) {
                const double t = double(i) / rate, p = std::fmod(t, .42);
                // Repeated tonal attacks have a stable IOI, but no drum evidence.
                const float sample = float(std::exp(-13 * p) * (.25 * std::sin(2 * std::numbers::pi * 261.63 * p) +
                    .10 * std::sin(2 * std::numbers::pi * 523.26 * p) +
                    .035 * std::sin(2 * std::numbers::pi * 1046.52 * p)));
                if (!analyzer.Push(sample, t + 1. / rate, frame)) continue;
                CHECK(frame.bpm == 0);
                for (unsigned e = 0; e < frame.count; ++e) {
                    onsets += frame.events[e].kind == RhythmKind::Onset;
                    beats += frame.events[e].kind == RhythmKind::Beat;
                }
            }
            CHECK(onsets >= 8 && beats == 0);
        }
        {
            MusicAnalyzer analyzer;
            analyzer.Configure(rate, 0);
            MusicFrame frame;
            constexpr double period = 60. / 174.;
            unsigned wrongOctave = 0, locked = 0, downbeats = 0;
            for (unsigned i = 0; i < 14 * rate; ++i) {
                const double t = double(i) / rate;
                const unsigned beat = unsigned(t / period);
                const double p = t - beat * period;
                const double q = p - .018;
                const double kick = p < .11 ? (beat % 4 == 0 ? .55 : .32) * std::exp(-28 * p) *
                    std::sin(2 * std::numbers::pi * (55 * p + 60 * p * p)) : 0;
                const double snare = beat % 2 && q >= 0 && q < .056 ? std::exp(-55 * q) *
                    (.3 * std::sin(2 * std::numbers::pi * 185 * q) + .10 * std::sin(2 * std::numbers::pi * 3100 * q)) : 0;
                const float sample = float(kick + snare + .045 * std::sin(2 * std::numbers::pi * 73 * t));
                if (!analyzer.Push(sample, t + 1. / rate, frame) || t < 5. || !frame.period) continue;
                ++locked;
                wrongOctave += frame.bpm < 100;
                for (unsigned e = 0; e < frame.count; ++e) {
                    if (frame.events[e].kind != RhythmKind::Downbeat) continue;
                    ++downbeats;
                    const double at = frame.events[e].at;
                    CHECK(std::abs(at - std::round(at / (4 * period)) * 4 * period) < .06);
                }
            }
            CHECK(locked > 800 && wrongOctave == 0 && std::abs(frame.bpm - 174.f) < 2.f);
            CHECK(downbeats > 0);
        }
        for (unsigned hz : {60, 120, 240}) {
            MusicQueue queue;
            MusicSync sync;
            unsigned beats = 0, kicks = 0;
            MusicEnvelope envelope;
            const auto deliver = [&](RhythmEvent event) {
                beats += event.kind == RhythmKind::Beat;
                kicks += event.kind == RhythmKind::Kick;
            };
            unsigned hop = 0;
            for (unsigned f = 0; f <= 2 * hz; ++f) {
                const double now = double(f) / hz;
                for (; hop <= 190 && hop * .01 <= now; ++hop) {
                    MusicFrame frame;
                    frame.generation = 1;
                    frame.at = hop * .01;
                    frame.period = .5;
                    frame.confidence = .8f;
                    if (hop % 50 == 0) {
                        frame.Add(RhythmKind::Kick, frame.at, .9f, .8f);
                        frame.Add(RhythmKind::Beat, frame.at + .02, .8f, .8f);
                    }
                    CHECK(queue.Push(frame));
                }
                MusicFrame incoming;
                while (queue.Pop(incoming)) sync.Read(incoming, now, deliver);
                sync.Advance(now, deliver);
                if (f < hz) envelope.Step(1.f, 1. / hz, .5f, .5f);
            }
            CHECK(beats == 4 && kicks == 4 && !queue.Overruns());
            CHECK(std::abs(envelope.value - (1.f - std::exp(-2.f))) < .00001f);
            CHECK(std::abs(sync.Frame().Phase(1., 4) - .5f) < .00001f);
            CHECK(std::abs(sync.Frame().Phase(1.5, 2) - .5f) < .00001f);
            CHECK(std::abs(sync.Frame().Phase(1., 8) - .25f) < .00001f);
            MusicFrame future;
            future.generation = 1;
            future.Add(RhythmKind::Beat, 20, 1, 1);
            sync.Read(future, 10, deliver);
            future = {};
            future.generation = 2;
            sync.Read(future, 11, deliver);
            sync.Advance(25, deliver);
            CHECK(beats == 4);
        }
        {
            BeatTracker tracker(0);
            tracker.EnableMusicTracking();
            for (unsigned hop = 0; hop < 800; ++hop)
                tracker.FeedFlux(hop % 50 == 0 ? 2.f : 0.f, false, .01f);
            CHECK(std::abs(tracker.Bpm() - 120.f) < 2.f);
            for (unsigned hop = 0; hop < 900; ++hop)
                tracker.FeedFlux(hop % 66 == 0 ? 2.f : 0.f, false, .01f);
            CHECK(std::abs(tracker.Bpm() - 60.f / .66f) < 2.f);
        }
        for (bool lowEndReturns : {true, false}) {
            MusicStructure structure;
            unsigned drops = 0;
            bool building = false;
            for (unsigned i = 0; i < 1100; ++i) {
                MusicFrame frame;
                frame.at = i * .01;
                const bool build = i >= 650 && i < 850;
                const bool after = i >= 850;
                const float power = build ? .015f + (i - 650) * .000075f : after ? .15f : .04f;
                const float bass = build ? power * .01f : after ? power * (lowEndReturns ? .5f : .01f) : .014f;
                const bool onset = i % (build ? 12 : 50) == 0;
                structure.Feed(frame, .01f, power, bass, onset, false);
                building |= frame.state == MusicState::Building;
                for (unsigned e = 0; e < frame.count; ++e)
                    if (frame.events[e].kind == RhythmKind::Drop) {
                        ++drops;
                        CHECK(frame.at >= 8.5 && frame.at < 9.);
                    }
            }
            CHECK(building && drops == unsigned(lowEndReturns));
        }
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("now_playing_hls_test: all passed\n");
    return 0;
}
