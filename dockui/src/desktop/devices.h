#pragma once
// Battery levels of this PC and of connected accessories, for the Batteries widget and the Menu Bar item.
// Sources (read-only, no drivers, no vendor software):
//   * Bluetooth devices that report their level to Windows (the value Settings > Bluetooth shows): the device
//     property {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2 on present device nodes (mice, keyboards, headsets, pens...);
//   * XInput controllers (Xbox wireless and compatible) through XInputGetBatteryInformation;
//   * this PC through GetSystemPowerStatus.
//   * accessories on 2.4 GHz receivers and USB through their HID interfaces (Razer, Logitech HID++, standard HID
//     battery strength): see hid_battery.h.
#include <windows.h>
#include <string>
#include <vector>

namespace docky::desk {

enum class DeviceKind { PC, Mouse, Keyboard, Headphones, Gamepad, Phone, Pen, Other };
struct DeviceBattery {
    std::wstring name;
    int percent = -1;        // 0..100, -1 unknown
    DeviceKind kind = DeviceKind::Other;
    bool charging = false;
};
DeviceKind GuessKind(const std::wstring& name, const std::wstring& deviceClass);  // unit-testable
const wchar_t* DeviceGlyph(DeviceKind kind);  // Segoe Fluent Icons / MDL2 code point

class DeviceBatteries {
public:
    void Start(HWND notify, UINT message);  // polls on a worker thread (every 60 s); message posted on change
    void Stop();
    void Refresh();
    std::vector<DeviceBattery> Snapshot() const;  // the PC first (when it has a battery), then accessories by name
};
DeviceBatteries& Devices();
std::vector<DeviceBattery> ReadDeviceBatteriesNow();  // one synchronous read (diagnostics)

}  // namespace docky::desk
