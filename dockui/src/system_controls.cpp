#include "system_controls.h"
#include "system_status.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <wlanapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <atomic>
#include <map>

#include "log.h"
#include "wifi_list.h"
#include <appmodel.h>

using Microsoft::WRL::ComPtr;

namespace liquid::controls {
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

void QueryRadios(State& s) {
    try {
        const auto access = wr::Radio::RequestAccessAsync().get();
        const bool allowed = access == wr::RadioAccessStatus::Allowed;
        for (const auto& r : wr::Radio::GetRadiosAsync().get()) {
            const auto st = r.State();
            const liquid::controls::Radio v = st == wr::RadioState::On ? liquid::controls::Radio::On
                                              : st == wr::RadioState::Off || st == wr::RadioState::Disabled ? liquid::controls::Radio::Off
                                                                                                            : liquid::controls::Radio::Unknown;
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

void QueryBluetoothDevices(State& s) {
    using namespace winrt::Windows::Devices;
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
