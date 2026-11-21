#pragma once
// System controls for Control Center: official Windows APIs only (Core Audio, WLAN API, WMI,
// Windows.Devices.Radios, Windows.Devices.Enumeration, Windows.Media.Control, Shell). No hooks,
// injection, simulated keys or elevation. Every function blocks; call them from a worker thread
// that has an initialised multithreaded (MTA) apartment. Capabilities that are missing or denied are
// reported as unavailable so the UI never shows a control that cannot work.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace liquid::controls {

enum class Radio { Unknown, On, Off, Unavailable };
inline bool CanToggleRadio(bool switchable,Radio state) { return switchable && (state==Radio::On || state==Radio::Off); }
enum class PowerSource { Unknown, Battery, External };
inline const wchar_t* BatterySourceLabel(bool charging, PowerSource source) {
    if (charging) return L"Charging";
    if (source == PowerSource::External) return L"Plugged In";
    if (source == PowerSource::Battery) return L"On Battery";
    return L"Power Source Unknown";
}

struct Network {
    std::wstring ssid;
    std::wstring profile;  // non-empty when Windows already has a saved profile (credentials)
    int signal = 0;        // 0..100
    bool secured = false, connected = false;
    int bssType = 1;       // DOT11_BSS_TYPE
    GUID adapter{};         // interface on which this network/profile was enumerated
};
inline void MergeNetwork(std::vector<Network>& networks,const Network& item) {
    for(auto& existing:networks)if(existing.ssid==item.ssid && existing.profile==item.profile &&
       existing.bssType==item.bssType && existing.secured==item.secured && IsEqualGUID(existing.adapter,item.adapter)) {
        existing.signal=std::max(existing.signal,item.signal);existing.connected=existing.connected||item.connected;return;
    }
    networks.push_back(item);
}

// A paired Bluetooth device as the menu bar's Bluetooth menu lists it (macOS: every paired device, connected ones
// highlighted, a battery level where Windows knows one).
enum class BtKind { Other, Keyboard, Mouse, Trackpad, Headphones, Speaker, Gamepad, Phone };
struct BtDevice {
    std::wstring name;
    bool connected = false;
    BtKind kind = BtKind::Other;
    int battery = -1;  // 0..100, -1 unknown
};
// Device kind from the Bluetooth Class of Device (classic) and the name (LE / unknown classes). Pure, unit tested.
BtKind BtKindOf(unsigned classOfDevice, const std::wstring& name);

struct State {
    // Wi-Fi (WLAN API; radio switch through Windows.Devices.Radios)
    bool wifiAdapter = false, wifiConnected = false;
    std::wstring ssid;
    Radio wifiRadio = Radio::Unavailable;
    bool wifiSwitchable = false;      // a Radios API Wi-Fi radio exists and access is allowed
    std::vector<Network> networks;    // filled only when requested
    // Bluetooth (Windows.Devices.Radios + Enumeration)
    Radio btRadio = Radio::Unavailable;
    bool btSwitchable = false;
    std::vector<std::wstring> btDevices;  // currently connected devices
    std::vector<BtDevice> btPaired;       // every paired device, sorted by name (filled with the device list)
    // Audio (Core Audio default render endpoint)
    bool audio = false, muted = false;
    int volume = 0;
    // Brightness (WMI WmiMonitorBrightness: integrated panels only)
    bool brightness = false;
    int brightnessPercent = 0;
    // Battery
    bool batteryKnown = false, battery = false, charging = false;
    PowerSource powerSource = PowerSource::Unknown;
    int batteryPercent = -1;
    // Media (GlobalSystemMediaTransportControls)
    bool media = false, playing = false, canPlayPause = false, canNext = false, canPrevious = false;
    std::wstring title, artist, source;
    std::shared_ptr<const std::vector<uint32_t>> artwork;
    uint32_t artworkWidth=0, artworkHeight=0;
    uint64_t artworkHash=0;
    // Notifications / Focus: read-only hint from SHQueryUserNotificationState
    int notificationState = 0;
};

State Query(bool includeNetworks, bool includeBluetoothDevices);

bool SetVolume(int percent);
bool SetMute(bool muted);
bool SetBrightness(int percent);
// wifi=true: Wi-Fi radio, false: Bluetooth radio. Uses Windows.Devices.Radios (user consent rules apply).
bool SetRadio(bool wifi, bool on, std::wstring& error);
void ScanNetworks();
// Connects with an existing saved profile only; networks without one go to the Windows flow.
bool ConnectNetwork(const Network& network, std::wstring& error);
enum class MediaCommand { PlayPause, Next, Previous };
bool SendMedia(MediaCommand command);
void OpenUri(const wchar_t* uri);

// Notifies `window` with `message` whenever the default endpoint's volume or mute changes.
class VolumeWatcher {
public:
    ~VolumeWatcher();
    bool Start(HWND window, UINT message);
    void Stop();

private:
    void* impl_ = nullptr;
};

}  // namespace liquid::controls
