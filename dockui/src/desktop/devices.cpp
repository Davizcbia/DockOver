#include "devices.h"
#include "hid_battery.h"
#include <initguid.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace docky::desk {

DeviceKind GuessKind(const std::wstring& name, const std::wstring& deviceClass) {
    std::wstring n = name + L" " + deviceClass;
    for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
    auto has = [&](const wchar_t* s) { return n.find(s) != std::wstring::npos; };
    if (has(L"controller") || has(L"gamepad") || has(L"xbox") || has(L"dualsense") || has(L"dualshock") || has(L"joy-con") || has(L"pro controller")) return DeviceKind::Gamepad;
    if (has(L"mouse") || has(L"mx master") || has(L"mx anywhere") || has(L"trackpad") || has(L"trackball")) return DeviceKind::Mouse;
    if (has(L"keyboard") || has(L"keys") || has(L" kb") || has(L"mx keys")) return DeviceKind::Keyboard;
    if (has(L"airpods") || has(L"buds") || has(L"headphone") || has(L"headset") || has(L"earbud") || has(L"wh-") || has(L"wf-") || has(L"beats") || has(L"audioendpoint")) return DeviceKind::Headphones;
    if (has(L"iphone") || has(L"phone") || has(L"galaxy") || has(L"pixel")) return DeviceKind::Phone;
    if (has(L"pen") || has(L"stylus")) return DeviceKind::Pen;
    return DeviceKind::Other;
}

const wchar_t* DeviceGlyph(DeviceKind k) {
    switch (k) {
    case DeviceKind::PC: return L"";          // laptop
    case DeviceKind::Mouse: return L"";
    case DeviceKind::Keyboard: return L"";
    case DeviceKind::Headphones: return L"";
    case DeviceKind::Gamepad: return L"";
    case DeviceKind::Phone: return L"";
    case DeviceKind::Pen: return L"";
    default: return L"";                      // Bluetooth
    }
}

namespace {
// Bluetooth battery level reported to Windows (DEVPROP_TYPE_BYTE, 0..100).
const DEVPROPKEY kBatteryLevel{{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
// No connection filter: {83DA6326-...} 15 read False for a connected Wave Keys keyboard (measured 2026-11-11), so
// paired devices are listed with the last level Windows recorded.

std::wstring StringProp(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0; wchar_t buf[512]{}; DWORD size = 0;
    if (SetupDiGetDevicePropertyW(set, &d, &key, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf) - sizeof(wchar_t), &size, 0) && type == DEVPROP_TYPE_STRING) return buf;
    return {};
}

GUID ContainerOf(HDEVINFO set, SP_DEVINFO_DATA& d) {
    DEVPROPTYPE type = 0; GUID g{}; DWORD size = 0;
    if (SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<BYTE*>(&g), sizeof(g), &size, 0) && type == DEVPROP_TYPE_GUID) return g;
    return GUID{};
}
struct GuidLess { bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a, &b, sizeof(GUID)) < 0; } };

std::vector<DeviceBattery> Read() {
    // Pass 1: what each physical device (container) consists of. A Bluetooth mouse's container also holds a node of
    // class "Mouse"; while it is disconnected those child nodes are not present, which also tells a stale level apart.
    struct Parts { int nodes = 0; bool mouse = false, keyboard = false, audio = false, gamepad = false; };
    std::map<GUID, Parts, GuidLess> containers;
    struct Candidate { std::wstring name, cls; BYTE level; GUID container; };
    std::vector<Candidate> candidates;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA d{}; d.cbSize = sizeof(d);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d) && i < 20000; ++i) {
            const GUID container = ContainerOf(set, d);
            const std::wstring cls = StringProp(set, d, DEVPKEY_Device_Class);
            auto& parts = containers[container];
            ++parts.nodes;
            std::wstring lower = cls; for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
            if (lower == L"mouse") parts.mouse = true;
            else if (lower == L"keyboard") parts.keyboard = true;
            else if (lower == L"audioendpoint" || lower == L"media") parts.audio = true;
            else if (lower == L"xnacomposite" || lower == L"xboxcomposite") parts.gamepad = true;
            DEVPROPTYPE type = 0; BYTE level = 0; DWORD size = 0;
            if (!SetupDiGetDevicePropertyW(set, &d, &kBatteryLevel, &type, &level, sizeof(level), &size, 0) || type != DEVPROP_TYPE_BYTE || level > 100) continue;
            std::wstring name = StringProp(set, d, DEVPKEY_Device_FriendlyName);
            if (name.empty()) name = StringProp(set, d, DEVPKEY_Device_DeviceDesc);
            if (!name.empty()) candidates.push_back({name, cls, level, container});
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    std::map<std::wstring, DeviceBattery> found;  // by name: several device nodes describe one accessory
    const GUID none{};
    for (const auto& c : candidates) {
        const bool grouped = memcmp(&c.container, &none, sizeof(GUID)) != 0;
        const Parts parts = grouped ? containers[c.container] : Parts{};
        if (grouped && parts.nodes <= 1) continue;  // only the Bluetooth node is present: disconnected, stale level
        DeviceKind kind = parts.gamepad ? DeviceKind::Gamepad : parts.mouse ? DeviceKind::Mouse : parts.keyboard ? DeviceKind::Keyboard : parts.audio ? DeviceKind::Headphones : DeviceKind::Other;
        if (kind == DeviceKind::Other) kind = GuessKind(c.name, c.cls);
        DeviceBattery b{c.name, c.level, kind, false};
        while (!b.name.empty() && b.name.back() == L' ') b.name.pop_back();
        auto it = found.find(b.name);
        if (it == found.end()) found.emplace(b.name, b);
        else if (it->second.kind == DeviceKind::Other) it->second.kind = b.kind;
    }
    std::vector<DeviceBattery> out;
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100)
        out.push_back({L"This PC", ps.BatteryLifePercent, DeviceKind::PC, (ps.BatteryFlag & 8) != 0});
    const bool bluetoothPad = std::any_of(found.begin(), found.end(), [](const auto& f) { return f.second.kind == DeviceKind::Gamepad; });
    for (DWORD user = 0; user < XUSER_MAX_COUNT && !bluetoothPad; ++user) {  // XInput controllers (wired ones have no battery to show); a
        // Bluetooth Xbox controller is already listed by name above
        XINPUT_STATE state{};
        if (XInputGetState(user, &state) != ERROR_SUCCESS) continue;
        XINPUT_BATTERY_INFORMATION info{};
        if (XInputGetBatteryInformation(user, BATTERY_DEVTYPE_GAMEPAD, &info) != ERROR_SUCCESS || info.BatteryType == BATTERY_TYPE_WIRED || info.BatteryType == BATTERY_TYPE_DISCONNECTED) continue;
        const int pct = info.BatteryLevel == BATTERY_LEVEL_FULL ? 100 : info.BatteryLevel == BATTERY_LEVEL_MEDIUM ? 60 : info.BatteryLevel == BATTERY_LEVEL_LOW ? 25 : 5;
        out.push_back({L"Controller " + std::to_wstring(user + 1), pct, DeviceKind::Gamepad, false});
    }
    for (auto& b : ReadHidBatteries()) {  // 2.4 GHz receivers and USB (Razer, Logitech HID++, standard HID battery)
        if (found.count(b.name)) continue;
        found.emplace(b.name, b);
    }
    for (auto& [name, b] : found) out.push_back(b);
    return out;
}

struct Shared {
    mutable std::mutex mutex; std::vector<DeviceBattery> list;
    HWND notify = nullptr; UINT message = 0; HANDLE wake = nullptr; std::atomic<bool> stop{false}; std::thread thread;
} g;
bool Same(const std::vector<DeviceBattery>& a, const std::vector<DeviceBattery>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].name != b[i].name || a[i].percent != b[i].percent || a[i].charging != b[i].charging) return false;
    return true;
}
}  // namespace

void DeviceBatteries::Start(HWND notify, UINT message) {
    if (g.thread.joinable()) return;
    g.notify = notify; g.message = message; g.stop = false;
    g.wake = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    g.thread = std::thread([] {
        while (!g.stop) {
            if (WaitForSingleObject(g.wake, 60 * 1000) == WAIT_FAILED || g.stop) break;
            auto next = Read();
            bool changed = false;
            { std::lock_guard<std::mutex> l(g.mutex); changed = !Same(g.list, next); g.list = std::move(next); }
            if (changed && g.notify) PostMessageW(g.notify, g.message, 0, 0);
        }
    });
}
void DeviceBatteries::Stop() { g.stop = true; if (g.wake) SetEvent(g.wake); if (g.thread.joinable()) g.thread.join(); if (g.wake) { CloseHandle(g.wake); g.wake = nullptr; } }
void DeviceBatteries::Refresh() { if (g.wake) SetEvent(g.wake); }
std::vector<DeviceBattery> DeviceBatteries::Snapshot() const { std::lock_guard<std::mutex> l(g.mutex); return g.list; }
DeviceBatteries& Devices() { static DeviceBatteries d; return d; }
std::vector<DeviceBattery> ReadDeviceBatteriesNow() { return Read(); }

}  // namespace docky::desk
