#pragma once
// Battery levels of accessories on 2.4 GHz receivers and USB, read straight from their HID interfaces (read-only queries,
// no drivers, no vendor software). Implemented from the public protocol descriptions:
//   * Razer (VID 1532): the 91-byte feature report protocol, "get battery level" class 0x07 id 0x80 and "get charging
//     status" 0x07 0x84 (verified on a Razer HyperSpeed dongle, PID 00B9);
//   * Logitech (VID 046D) Unifying / Bolt / Lightspeed receivers and direct devices: HID++ 2.0 long reports on the vendor
//     collection FF00:0002, features 0x1004 (unified battery), 0x1000 (battery status) or 0x1001 (voltage), with the
//     name and type from 0x0005;
//   * any other HID device with a standard Battery Strength value (usage page 0x06, usage 0x20) in a feature report.
#include "devices.h"
#include <vector>

namespace docky::desk {

std::vector<DeviceBattery> ReadHidBatteries();

}  // namespace docky::desk
