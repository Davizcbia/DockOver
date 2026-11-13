// Headless checks of the Control Center system layer on the real machine.
// Safe by design: volume and mute are changed and restored immediately; brightness is only rewritten
// with its current value; radios are never switched; Wi-Fi is never (dis)connected.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <winrt/base.h>

#include "system_controls.h"

using namespace liquid::controls;

static int g_fail = 0;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                \
            ++g_fail;                                                       \
        }                                                                   \
    } while (0)

static const char* RadioName(Radio r) {
    return r == Radio::On ? "on" : r == Radio::Off ? "off" : r == Radio::Unknown ? "unknown" : "unavailable";
}

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    CHECK(CanToggleRadio(true,Radio::On) && CanToggleRadio(true,Radio::Off));
    CHECK(!CanToggleRadio(true,Radio::Unknown) && !CanToggleRadio(true,Radio::Unavailable) && !CanToggleRadio(false,Radio::On));
    CHECK(std::wstring(BatterySourceLabel(true, PowerSource::External)) == L"Charging");
    CHECK(std::wstring(BatterySourceLabel(false, PowerSource::External)) == L"Plugged In");
    CHECK(std::wstring(BatterySourceLabel(false, PowerSource::Battery)) == L"On Battery");
    CHECK(std::wstring(BatterySourceLabel(false, PowerSource::Unknown)) == L"Power Source Unknown");
    { Network a; a.ssid=L"same"; a.profile=L"first"; a.signal=30; a.adapter.Data1=1;
      Network b=a; b.profile=L"second"; b.signal=70; Network c=a; c.signal=80; c.connected=true;
      Network d=a; d.adapter.Data1=2; std::vector<Network> list; MergeNetwork(list,a);MergeNetwork(list,b);MergeNetwork(list,c);MergeNetwork(list,d);
      CHECK(list.size()==3 && list[0].profile==L"first" && list[0].signal==80 && list[0].connected && list[1].profile==L"second" && list[2].adapter.Data1==2); }
    if (g_fail) return 1;
    // Helper modes for verify_control_center.ps1: --read prints volume/mute; --set-volume N / --set-mute 0|1 / --set-brightness N write them.
    if (argc >= 2 && std::wstring(argv[1]) == L"--read") {
        State r = Query(false, false);
        std::printf("volume=%d muted=%d brightness=%d/%d\n", r.volume, r.muted, r.brightness, r.brightnessPercent);
        return 0;
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--set-volume") return SetVolume(_wtoi(argv[2])) ? 0 : 1;
    if (argc >= 3 && std::wstring(argv[1]) == L"--set-mute") return SetMute(_wtoi(argv[2]) != 0) ? 0 : 1;
    if (argc >= 3 && std::wstring(argv[1]) == L"--set-brightness") return SetBrightness(_wtoi(argv[2])) ? 0 : 1;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    State s = Query(true, true);
    QueryPerformanceCounter(&t1);
    std::printf("query %.0f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    std::printf("wifi adapter=%d radio=%s switchable=%d connected=%d ssidLen=%zu networks=%zu (with saved profile: %zu)\n",
                s.wifiAdapter, RadioName(s.wifiRadio), s.wifiSwitchable, s.wifiConnected, s.ssid.size(), s.networks.size(),
                static_cast<size_t>(std::count_if(s.networks.begin(), s.networks.end(), [](const Network& n) { return !n.profile.empty(); })));
    std::printf("bluetooth radio=%s switchable=%d connectedDevices=%zu\n", RadioName(s.btRadio), s.btSwitchable, s.btDevices.size());
    std::printf("audio=%d volume=%d muted=%d\n", s.audio, s.volume, s.muted);
    std::printf("brightness supported=%d percent=%d\n", s.brightness, s.brightnessPercent);
    std::printf("battery=%d percent=%d charging=%d\n", s.battery, s.batteryPercent, s.charging);
    std::printf("media=%d playing=%d playPause=%d next=%d prev=%d titleLen=%zu\n", s.media, s.playing, s.canPlayPause, s.canNext,
                s.canPrevious, s.title.size());
    std::printf("notification state=%d\n", s.notificationState);
    if (s.wifiConnected) CHECK(!s.ssid.empty());

    if (s.audio) {
        const int original = s.volume;
        const bool originalMute = s.muted;
        const int probe = original == 37 ? 38 : 37;
        CHECK(SetVolume(probe));
        State a = Query(false, false);
        CHECK(a.volume == probe);
        CHECK(SetMute(!originalMute));
        State b = Query(false, false);
        CHECK(b.muted == !originalMute);
        CHECK(SetMute(originalMute));
        CHECK(SetVolume(original));
        State c = Query(false, false);
        CHECK(c.volume == original && c.muted == originalMute);
        std::printf("volume round trip: set %d -> read %d, mute toggled and restored, back to %d\n", probe, a.volume, c.volume);
    }
    if (s.brightness) {
        CHECK(SetBrightness(s.brightnessPercent));  // same value: verifies the WMI write path without a visible change
        State d = Query(false, false);
        CHECK(d.brightness && d.brightnessPercent == s.brightnessPercent);
        std::printf("brightness write path ok (rewrote %d%%)\n", s.brightnessPercent);
    }
    std::printf(g_fail ? "system controls: %d FAILED\n" : "system controls: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
