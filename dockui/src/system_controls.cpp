#include "system_controls.h"
#include "system_status.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <wlanapi.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <mutex>
#pragma comment(lib, "setupapi.lib")
#include <wrl/client.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <map>

#include "log.h"
#include "wifi_list.h"
#include <appmodel.h>

using Microsoft::WRL::ComPtr;

namespace liquid::controls {

BtKind BtKindOf(unsigned cod, const std::wstring& name) {
    std::wstring n = name; for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
    auto has = [&](const wchar_t* w) { return n.find(w) != std::wstring::npos; };
    if (has(L"trackpad") || has(L"touchpad")) return BtKind::Trackpad;
    const unsigned major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    if (major == 2) return BtKind::Phone;
    if (major == 4) return minor == 5 || minor == 7 ? BtKind::Speaker : BtKind::Headphones;  // loudspeaker, portable audio
    if (major == 5) {  // peripheral: upper two minor bits keyboard / pointing, lower four joystick / gamepad
        if ((minor & 0x0F) == 1 || (minor & 0x0F) == 2) return BtKind::Gamepad;
        if (minor & 0x10) return BtKind::Keyboard;
        if (minor & 0x20) return BtKind::Mouse;
    }
    if (has(L"keyboard") || has(L"keys") || has(L" kb") || has(L"k380") || has(L"k780")) return BtKind::Keyboard;
    if (has(L"mouse") || has(L"mx master") || has(L"triathlon")) return BtKind::Mouse;
    if (has(L"controller") || has(L"gamepad") || has(L"playstation") || has(L"dualsense") || has(L"xbox")) return BtKind::Gamepad;
    if (has(L"buds") || has(L"pods") || has(L"headphone") || has(L"headset") || has(L"earbud") || has(L"momentum") || has(L"wh-1000") ||
        has(L"wf-1000") || has(L"quietcomfort") || has(L"beats")) return BtKind::Headphones;
    if (has(L"speaker") || has(L"soundbar") || has(L"boom")) return BtKind::Speaker;
    if (has(L"phone")) return BtKind::Phone;
    return BtKind::Other;
}

namespace {

ComPtr<IAudioEndpointVolume> Endpoint() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) return nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf()))))
        return nullptr;
    return volume;
}

std::wstring Ssid(const DOT11_SSID& s) {
    std::string bytes(reinterpret_cast<const char*>(s.ucSSID), std::min<ULONG>(s.uSSIDLength, DOT11_SSID_MAX_LENGTH));
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return out;
}

struct Wlan {
    HANDLE h = nullptr;
    GUID guid{};
    bool ok = false;
    Wlan() {
        DWORD negotiated = 0;
        if (WlanOpenHandle(2, nullptr, &negotiated, &h) != ERROR_SUCCESS) return;
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
            if (list->dwNumberOfItems) {
                guid = list->InterfaceInfo[0].InterfaceGuid;
                for (DWORD i = 0; i < list->dwNumberOfItems; ++i)
                    if (list->InterfaceInfo[i].isState == wlan_interface_state_connected) guid = list->InterfaceInfo[i].InterfaceGuid;
                ok = true;
            }
            WlanFreeMemory(list);
        }
    }
    ~Wlan() {
        if (h) WlanCloseHandle(h, nullptr);
    }
};

// Packaged (sparse identity) processes get no WLAN network list without location access; see wifi_list.h.
bool Packaged() {
    UINT32 n = 0;
    return GetCurrentPackageFullName(&n, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}
// Runs docky-wifi-list.exe (next to this exe) and returns its stdout; false on any failure or after timeoutMs.
bool RunWifiHelper(bool scan, std::string& output, DWORD timeoutMs) {
    output.clear();
    wchar_t self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
    std::wstring exe(self);
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"docky-wifi-list.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return false;  // large buffer: the child never blocks on write
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + exe + L"\"" + (scan ? L" --scan" : L"");
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) { CloseHandle(readEnd); return false; }
    const bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    char buffer[4096];
    DWORD got = 0, avail = 0;
    while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got)
        output.append(buffer, got);
    CloseHandle(readEnd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return done && code == 0;
}

void QueryWifi(State& s, bool includeNetworks) {
    Wlan w;
    if (!w.ok) return;
    s.wifiAdapter = true;
    DWORD size = 0;
    PWLAN_RADIO_STATE radio = nullptr;
    if (WlanQueryInterface(w.h, &w.guid, wlan_intf_opcode_radio_state, nullptr, &size, reinterpret_cast<PVOID*>(&radio), nullptr) ==
            ERROR_SUCCESS &&
        radio) {
        bool on = false;
        for (DWORD i = 0; i < radio->dwNumberOfPhys; ++i)
            if (radio->PhyRadioState[i].dot11SoftwareRadioState == dot11_radio_state_on &&
                radio->PhyRadioState[i].dot11HardwareRadioState == dot11_radio_state_on)
                on = true;
        s.wifiRadio = on ? Radio::On : Radio::Off;
        WlanFreeMemory(radio);
    } else {
        s.wifiRadio = Radio::Unknown;
    }
    PWLAN_CONNECTION_ATTRIBUTES conn = nullptr;
    if (WlanQueryInterface(w.h, &w.guid, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<PVOID*>(&conn), nullptr) ==
            ERROR_SUCCESS &&
        conn) {
        if (conn->isState == wlan_interface_state_connected) {
            s.wifiConnected = true;
            s.ssid = Ssid(conn->wlanAssociationAttributes.dot11Ssid);
        }
        WlanFreeMemory(conn);
    }
    if (!includeNetworks) return;
    std::vector<Network> best;
    const DWORD rc = wifilist::List(w.h, w.guid, best);
    if ((rc != ERROR_SUCCESS || best.empty()) && Packaged()) {
        std::string text;
        std::vector<Network> viaHelper;
        const bool ok = RunWifiHelper(false, text, 4000) && wifilist::Decode(text, viaHelper);
        static int logged = 0;
        if (logged++ < 3) DLOG(L"wifi: in-process list rc=%lu count=%zu; helper %ls count=%zu", rc, best.size(), ok ? L"ok" : L"failed", viaHelper.size());
        if (ok) best = std::move(viaHelper);
    }
    if (best.empty() && rc != ERROR_SUCCESS) return;
    s.networks = std::move(best);
    std::sort(s.networks.begin(), s.networks.end(), [](const Network& a, const Network& b) {
        if (a.connected != b.connected) return a.connected;
        if (a.profile.empty() != b.profile.empty()) return !a.profile.empty();
        return a.signal > b.signal;
    });
}

namespace wr = winrt::Windows::Devices::Radios;

liquid::controls::Radio RadioState(wr::RadioState value) {
    if (value == wr::RadioState::On) return liquid::controls::Radio::On;
    if (value == wr::RadioState::Off || value == wr::RadioState::Disabled) return liquid::controls::Radio::Off;
    return liquid::controls::Radio::Unknown;
}

void QueryRadios(State& s) {
    try {
        const auto access = wr::Radio::RequestAccessAsync().get();
        const bool allowed = access == wr::RadioAccessStatus::Allowed;
        for (const auto& r : wr::Radio::GetRadiosAsync().get()) {
            const auto st = r.State();
            const auto v = RadioState(st);
            if (r.Kind() == wr::RadioKind::Bluetooth) {
                s.btRadio = v;
                s.btSwitchable = allowed && st != wr::RadioState::Disabled;
            } else if (r.Kind() == wr::RadioKind::WiFi) {
                s.wifiSwitchable = allowed && st != wr::RadioState::Disabled;
                if (s.wifiRadio == liquid::controls::Radio::Unknown || s.wifiRadio == liquid::controls::Radio::Unavailable) s.wifiRadio = v;
            }
        }
    } catch (const winrt::hresult_error& e) {
        DLOG(L"controls: radios unavailable 0x%08X", static_cast<unsigned>(e.code().value));
    }
}

// Battery levels Windows recorded for present device nodes (the Bluetooth battery property, 0..100), by friendly name.
std::map<std::wstring, int> BluetoothBatteryLevels() {
    static const DEVPROPKEY kBatteryLevel{{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
    std::map<std::wstring, int> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{}; d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d) && i < 20000; ++i) {
        DEVPROPTYPE type = 0; BYTE level = 0; DWORD size = 0;
        if (!SetupDiGetDevicePropertyW(set, &d, &kBatteryLevel, &type, &level, sizeof(level), &size, 0) || type != DEVPROP_TYPE_BYTE || level > 100) continue;
        wchar_t name[512]{};
        if (!SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_FriendlyName, &type, reinterpret_cast<BYTE*>(name), sizeof(name) - sizeof(wchar_t), &size, 0) || type != DEVPROP_TYPE_STRING) continue;
        std::wstring n = name; while (!n.empty() && n.back() == L' ') n.pop_back();
        if (!n.empty()) out[n] = level;
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

void QueryBluetoothDevices(State& s) {
    using namespace winrt::Windows::Devices;
    // Kinds come from the device itself (Class of Device / LE appearance) and are cached by device id: one lookup each.
    static std::mutex kindsMutex; static std::map<std::wstring, BtKind> kinds;
    auto kindOf = [&](const winrt::hstring& id, const std::wstring& name, bool le) {
        { std::lock_guard<std::mutex> lock(kindsMutex); auto it = kinds.find(id.c_str()); if (it != kinds.end()) return it->second; }
        BtKind kind = BtKindOf(0, name);
        // Some ids from the pairing selector are refused by FromIdAsync (E_INVALIDARG, seen for a paired headset); the
        // device address at the end of the id ("...-aa:bb:cc:dd:ee:ff") still finds it.
        auto address = [&]() -> uint64_t {
            const std::wstring s(id.c_str()); const size_t dash = s.rfind(L'-');
            if (dash == std::wstring::npos) return 0;
            uint64_t v = 0; int digits = 0;
            for (size_t i = dash + 1; i < s.size(); ++i) { const wchar_t c = s[i]; if (c == L':') continue; if (!iswxdigit(c)) return 0; v = v * 16 + (iswdigit(c) ? c - L'0' : (towlower(c) - L'a' + 10)); ++digits; }
            return digits == 12 ? v : 0;
        };
        auto classic = [&]() -> Bluetooth::BluetoothDevice {
            try { if (auto dev = Bluetooth::BluetoothDevice::FromIdAsync(id).get()) return dev; } catch (const winrt::hresult_error&) {}
            if (const uint64_t a = address()) return Bluetooth::BluetoothDevice::FromBluetoothAddressAsync(a).get();
            return nullptr;
        };
        try {
            if (!le) { if (auto dev = classic()) { const unsigned cod = dev.ClassOfDevice().RawValue(); kind = BtKindOf(cod, name); if (kind == BtKind::Other) DLOG(L"controls: bluetooth class of device 0x%06X", cod); } }
            else if (auto dev = Bluetooth::BluetoothLEDevice::FromIdAsync(id).get()) {
                const unsigned raw = dev.Appearance().RawValue();  // HID category 0x03C0: 1 keyboard, 2 mouse, 3 joystick, 4 gamepad
                if ((raw >> 6) == 0x0F) { const unsigned sub = raw & 0x3F; kind = sub == 1 ? BtKind::Keyboard : sub == 2 ? (kind == BtKind::Trackpad ? kind : BtKind::Mouse) : (sub == 3 || sub == 4) ? BtKind::Gamepad : kind; }
            }
        } catch (const winrt::hresult_error& e) { DLOG(L"controls: bluetooth kind lookup failed 0x%08X", static_cast<unsigned>(e.code().value)); }
        if (kind == BtKind::Other) DLOG(L"controls: bluetooth device kind unknown (le=%d)", le ? 1 : 0);
        std::lock_guard<std::mutex> lock(kindsMutex); kinds[id.c_str()] = kind;
        return kind;
    };
    try {
        for (const winrt::hstring& selector :
             {Bluetooth::BluetoothDevice::GetDeviceSelectorFromConnectionStatus(Bluetooth::BluetoothConnectionStatus::Connected),
              Bluetooth::BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(Bluetooth::BluetoothConnectionStatus::Connected)}) {
            for (const auto& d : Enumeration::DeviceInformation::FindAllAsync(selector).get()) {
                std::wstring name(d.Name().c_str());
                if (!name.empty() && std::find(s.btDevices.begin(), s.btDevices.end(), name) == s.btDevices.end())
                    s.btDevices.push_back(name);
            }
        }
        // Every paired device (classic and LE; one accessory may be both: merged by name), connected as found above.
        const auto levels = BluetoothBatteryLevels();
        for (int le = 0; le < 2; ++le) {
            const auto selector = le ? Bluetooth::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true) : Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true);
            for (const auto& d : Enumeration::DeviceInformation::FindAllAsync(selector).get()) {
                std::wstring name(d.Name().c_str());
                while (!name.empty() && name.back() == L' ') name.pop_back();
                if (name.empty()) continue;
                const BtKind kind = kindOf(d.Id(), name, le != 0);
                auto it = std::find_if(s.btPaired.begin(), s.btPaired.end(), [&](const BtDevice& b) { return b.name == name; });
                if (it == s.btPaired.end()) { s.btPaired.push_back(BtDevice{name, false, kind, -1}); it = s.btPaired.end() - 1; }
                else if (it->kind == BtKind::Other) it->kind = kind;
            }
        }
        for (auto& b : s.btPaired) {
            b.connected = std::find(s.btDevices.begin(), s.btDevices.end(), b.name) != s.btDevices.end();
            if (auto lv = levels.find(b.name); b.connected && lv != levels.end()) b.battery = lv->second;
        }
        std::sort(s.btPaired.begin(), s.btPaired.end(), [](const BtDevice& a, const BtDevice& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    } catch (const winrt::hresult_error& e) {
        DLOG(L"controls: bluetooth devices unavailable 0x%08X", static_cast<unsigned>(e.code().value));
    }
}

// WMI root\WMI brightness of integrated panels (external monitors are not reported here).
ComPtr<IWbemServices> WmiServices() {
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) return nullptr;
    ComPtr<IWbemServices> services;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    const HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(ns);
    if (FAILED(hr)) return nullptr;
    CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return services;
}

bool QueryBrightness(int& percent) {
    auto services = WmiServices();
    if (!services) return false;
    ComPtr<IEnumWbemClassObject> e;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT CurrentBrightness, Active FROM WmiMonitorBrightness");
    const HRESULT hr = services->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &e);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !e) return false;
    ComPtr<IWbemClassObject> obj;
    ULONG got = 0;
    bool found = false;
    while (e->Next(2000, 1, &obj, &got) == WBEM_S_NO_ERROR && got) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && (v.vt == VT_UI1 || v.vt == VT_I4)) {
            percent = v.vt == VT_UI1 ? v.bVal : v.lVal;
            found = true;
        }
        VariantClear(&v);
        obj.Reset();
        if (found) break;
    }
    return found;
}

}  // namespace

Radio ReadBluetoothRadio() {
    try {
        for (const auto& radio : wr::Radio::GetRadiosAsync().get())
            if (radio.Kind() == wr::RadioKind::Bluetooth) return RadioState(radio.State());
    } catch (const winrt::hresult_error&) {}
    return Radio::Unavailable;
}

State Query(bool includeNetworks, bool includeBluetoothDevices) {
    State s;
    QueryWifi(s, includeNetworks);
    QueryRadios(s);
    if (includeBluetoothDevices && s.btRadio == Radio::On) QueryBluetoothDevices(s);
    if (auto v = Endpoint()) {
        float level = 0;
        BOOL muted = FALSE;
        if (SUCCEEDED(v->GetMasterVolumeLevelScalar(&level)) && SUCCEEDED(v->GetMute(&muted))) {
            s.audio = true;
            s.muted = muted != FALSE;
            s.volume = std::clamp(static_cast<int>(level * 100.0f + 0.5f), 0, 100);
        }
    }
    s.brightness = QueryBrightness(s.brightnessPercent);
    SYSTEM_POWER_STATUS power{};
    const bool powerOk = GetSystemPowerStatus(&power) != FALSE;
    const auto decoded = liquid::status::DecodePower(powerOk, power.ACLineStatus, power.BatteryFlag, power.BatteryLifePercent);
    s.batteryKnown = decoded.known; s.battery = decoded.present; s.charging = decoded.charging; s.batteryPercent = decoded.percent;
    s.powerSource = decoded.source == liquid::status::PowerSource::AC ? PowerSource::External :
                    decoded.source == liquid::status::PowerSource::Battery ? PowerSource::Battery : PowerSource::Unknown;
    try {
        using namespace winrt::Windows::Media::Control;
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (const auto session = manager.GetCurrentSession()) {
            s.media = true;
            s.source = session.SourceAppUserModelId().c_str();
            const auto info = session.GetPlaybackInfo();
            s.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            const auto c = info.Controls();
            s.canPlayPause = c.IsPlayPauseToggleEnabled();
            s.canNext = c.IsNextEnabled();
            s.canPrevious = c.IsPreviousEnabled();
            try {
                const auto props = session.TryGetMediaPropertiesAsync().get();
                s.title = props.Title().c_str();
                s.artist = props.Artist().c_str();
                try {
                    using namespace winrt::Windows::Graphics::Imaging;
                    if (auto thumbnail=props.Thumbnail()) {
                        auto stream=thumbnail.OpenReadAsync().get();
                        if (stream.Size() <= 16*1024*1024) {
                            auto decoder=BitmapDecoder::CreateAsync(stream).get();
                            const auto w=decoder.PixelWidth(), h=decoder.PixelHeight();
                            if(w && h) {
                                const float ratio=256.f/static_cast<float>(std::max(w,h));
                                BitmapTransform transform;
                                const uint32_t aw=std::max(1u,static_cast<uint32_t>(w*ratio)), ah=std::max(1u,static_cast<uint32_t>(h*ratio));
                                transform.ScaledWidth(aw);transform.ScaledHeight(ah);
                                auto bytes=decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Premultiplied,transform,
                                    ExifOrientationMode::IgnoreExifOrientation,ColorManagementMode::ColorManageToSRgb).get().DetachPixelData();
                                if(bytes.size()==static_cast<size_t>(aw)*ah*4) {
                                    auto pixels=std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(aw)*ah);
                                    std::memcpy(pixels->data(),bytes.data(),bytes.size());
                                    uint64_t hash=1469598103934665603ull;
                                    for(auto byte:bytes){hash^=byte;hash*=1099511628211ull;}
                                    s.artwork=pixels;s.artworkWidth=aw;s.artworkHeight=ah;s.artworkHash=hash;
                                }
                            }
                        }
                    }
                } catch (...) { /* Missing or unsupported artwork leaves the glass card usable. */ }

            } catch (...) {
            }
        }
    } catch (const winrt::hresult_error&) {
    }
    QUERY_USER_NOTIFICATION_STATE n{};
    if (SUCCEEDED(SHQueryUserNotificationState(&n))) s.notificationState = static_cast<int>(n);
    return s;
}

bool SetVolume(int percent) {
    auto v = Endpoint();
    return v && SUCCEEDED(v->SetMasterVolumeLevelScalar(std::clamp(percent, 0, 100) / 100.0f, nullptr));
}

bool SetMute(bool muted) {
    auto v = Endpoint();
    return v && SUCCEEDED(v->SetMute(muted ? TRUE : FALSE, nullptr));
}

bool SetBrightness(int percent) {
    auto services = WmiServices();
    if (!services) return false;
    ComPtr<IEnumWbemClassObject> e;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    HRESULT hr = services->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &e);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !e) return false;
    ComPtr<IWbemClassObject> instance, cls, inDef, in;
    ULONG got = 0;
    if (e->Next(2000, 1, &instance, &got) != WBEM_S_NO_ERROR || !got) return false;
    VARIANT path;
    VariantInit(&path);
    if (FAILED(instance->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) return false;
    BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
    bool ok = false;
    if (SUCCEEDED(services->GetObject(className, 0, nullptr, &cls, nullptr)) && SUCCEEDED(cls->GetMethod(method, 0, &inDef, nullptr)) &&
        SUCCEEDED(inDef->SpawnInstance(0, &in))) {
        VARIANT timeout, level;
        VariantInit(&timeout);
        VariantInit(&level);
        timeout.vt = VT_I4;
        timeout.lVal = 0;
        level.vt = VT_UI1;
        level.bVal = static_cast<BYTE>(std::clamp(percent, 0, 100));
        in->Put(L"Timeout", 0, &timeout, 0);
        in->Put(L"Brightness", 0, &level, 0);
        ok = SUCCEEDED(services->ExecMethod(path.bstrVal, method, 0, nullptr, in.Get(), nullptr, nullptr));
    }
    SysFreeString(className);
    SysFreeString(method);
    VariantClear(&path);
    return ok;
}

bool SetRadio(bool wifi, bool on, std::wstring& error) {
    try {
        if (wr::Radio::RequestAccessAsync().get() != wr::RadioAccessStatus::Allowed) {
            error = L"Windows did not allow radio control";
            return false;
        }
        for (const auto& r : wr::Radio::GetRadiosAsync().get()) {
            if (r.Kind() != (wifi ? wr::RadioKind::WiFi : wr::RadioKind::Bluetooth)) continue;
            const auto result = r.SetStateAsync(on ? wr::RadioState::On : wr::RadioState::Off).get();
            if (result == wr::RadioAccessStatus::Allowed) return true;
            error = result == wr::RadioAccessStatus::DeniedBySystem ? L"Denied by the system (e.g. Airplane Mode)" : L"Denied";
            return false;
        }
        error = L"No radio of this kind";
    } catch (const winrt::hresult_error& e) {
        error = e.message().c_str();
    }
    return false;
}

void ScanNetworks() {
    if (Packaged()) {  // scan through the helper (see wifi_list.h); fire and forget so the cached list shows at once
        wchar_t self[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
        std::wstring exe(self);
        exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"docky-wifi-list.exe";
        std::wstring cmd = L"\"" + exe + L"\" --scan";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return;
    }
    Wlan w;
    if (w.ok) WlanScan(w.h, &w.guid, nullptr, nullptr, nullptr);
}

bool ConnectNetwork(const Network& network, std::wstring& error) {
    if (network.profile.empty()) {
        error = L"No saved profile";
        return false;
    }
    Wlan w;
    if (!w.ok) {
        error = L"No Wi-Fi adapter";
        return false;
    }
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = network.profile.c_str();
    p.dot11BssType = static_cast<DOT11_BSS_TYPE>(network.bssType);
    const GUID empty{};
    if (IsEqualGUID(network.adapter,empty)) { error = L"Network adapter is unavailable; refresh the list."; return false; }
    const DWORD rc = WlanConnect(w.h, &network.adapter, &p, nullptr);
    if (rc != ERROR_SUCCESS) error = L"WlanConnect failed " + std::to_wstring(rc);
    return rc == ERROR_SUCCESS;
}

bool SendMedia(MediaCommand command) {
    using namespace winrt::Windows::Media::Control;
    try {
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const auto session = manager.GetCurrentSession();
        if (!session) return false;
        switch (command) {
        case MediaCommand::PlayPause: return session.TryTogglePlayPauseAsync().get();
        case MediaCommand::Next: return session.TrySkipNextAsync().get();
        case MediaCommand::Previous: return session.TrySkipPreviousAsync().get();
        }
    } catch (const winrt::hresult_error&) {
    }
    return false;
}

void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------------------------------------
namespace {
class VolumeCallback final : public IAudioEndpointVolumeCallback {
public:
    VolumeCallback(HWND w, UINT m) : window_(w), message_(m) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        PostMessageW(window_, message_, 0, 0);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    HWND window_;
    UINT message_;
};
struct WatcherImpl {
    ComPtr<IAudioEndpointVolume> endpoint;
    VolumeCallback* callback = nullptr;
};
}  // namespace

VolumeWatcher::~VolumeWatcher() { Stop(); }

bool VolumeWatcher::Start(HWND window, UINT message) {
    Stop();
    auto* impl = new WatcherImpl();
    impl->endpoint = Endpoint();
    if (!impl->endpoint) {
        delete impl;
        return false;
    }
    impl->callback = new VolumeCallback(window, message);
    if (FAILED(impl->endpoint->RegisterControlChangeNotify(impl->callback))) {
        impl->callback->Release();
        delete impl;
        return false;
    }
    impl_ = impl;
    return true;
}

void VolumeWatcher::Stop() {
    if (!impl_) return;
    auto* impl = static_cast<WatcherImpl*>(impl_);
    impl->endpoint->UnregisterControlChangeNotify(impl->callback);
    impl->callback->Release();
    delete impl;
    impl_ = nullptr;
}

}  // namespace liquid::controls
