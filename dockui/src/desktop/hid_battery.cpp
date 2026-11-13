#include "hid_battery.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace docky::desk {
namespace {

struct Interface {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    USHORT usagePage = 0, usage = 0;
    USHORT inputLen = 0, outputLen = 0, featureLen = 0;
    std::wstring product;
};

std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }

HANDLE OpenQuery(const std::wstring& path) {  // no read/write access: attributes, strings and feature reports only
    return CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::vector<Interface> Enumerate() {
    std::vector<Interface> out;
    GUID hid{}; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA itf{}; itf.cbSize = sizeof(itf);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &itf) && i < 512; ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        Interface f; f.path = detail->DevicePath;
        HANDLE h = OpenQuery(f.path);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{}; a.Size = sizeof(a);
        if (HidD_GetAttributes(h, &a)) { f.vid = a.VendorID; f.pid = a.ProductID; }
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                f.usagePage = caps.UsagePage; f.usage = caps.Usage;
                f.inputLen = caps.InputReportByteLength; f.outputLen = caps.OutputReportByteLength; f.featureLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        wchar_t name[128]{};
        if (HidD_GetProductString(h, name, sizeof(name))) f.product = name;
        CloseHandle(h);
        out.push_back(std::move(f));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

std::wstring CleanName(std::wstring n) {
    for (const wchar_t* tail : {L" Wireless Receiver", L" Receiver", L" Dongle", L" Wireless"}) {
        const size_t len = wcslen(tail);
        if (n.size() > len && Lower(n.substr(n.size() - len)) == Lower(tail)) n.resize(n.size() - len);
    }
    while (!n.empty() && n.back() == L' ') n.pop_back();
    return n;
}

// The kind from the name, else from the device's other collections (a mouse also exposes a keyboard for its macros).
DeviceKind KindOf(const std::vector<Interface>& all, USHORT vid, USHORT pid, const std::wstring& name) {
    const DeviceKind guess = GuessKind(name, L"");
    if (guess != DeviceKind::Other) return guess;
    bool mouse = false, keyboard = false, pad = false;
    for (const auto& f : all) {
        if (f.vid != vid || f.pid != pid || f.usagePage != 0x01) continue;
        mouse |= f.usage == 0x02; keyboard |= f.usage == 0x06; pad |= f.usage == 0x04 || f.usage == 0x05;
    }
    return mouse ? DeviceKind::Mouse : pad ? DeviceKind::Gamepad : keyboard ? DeviceKind::Keyboard : DeviceKind::Other;
}

// ---- Razer -------------------------------------------------------------------------------------------------------
// 91-byte feature report incl. report id 0: [2] transaction id, [6] data size, [7] command class, [8] command id,
// [9..] arguments, [89] XOR of bytes 3..88. Reply [1] status: 02 ok, 01 busy, 03 failure, 04 timeout, 05 unsupported.
bool RazerQuery(HANDLE h, BYTE tid, BYTE cls, BYTE id, BYTE reply[91]) {
    BYTE b[91]{};
    b[2] = tid; b[6] = 0x02; b[7] = cls; b[8] = id;
    BYTE crc = 0; for (int i = 3; i <= 88; ++i) crc ^= b[i];
    b[89] = crc;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!HidD_SetFeature(h, b, sizeof(b))) return false;
        Sleep(60 + attempt * 40);
        memset(reply, 0, 91);
        if (!HidD_GetFeature(h, reply, 91)) return false;
        if (reply[1] == 0x01) continue;  // busy: ask again
        return reply[1] == 0x02 && reply[7] == cls && reply[8] == id;
    }
    return false;
}

// A sleeping mouse lets its dongle answer "timeout" (04): keep showing its last level for a while instead of dropping it.
struct Remembered { DeviceBattery battery; ULONGLONG tick = 0; };
std::map<DWORD, Remembered> g_razerLast;  // worker thread only
constexpr ULONGLONG kRazerKeep = 45ull * 60 * 1000;

void ReadRazer(const std::vector<Interface>& all, std::vector<DeviceBattery>& out) {
    std::map<DWORD, bool> done;
    for (const auto& f : all) {
        if (f.vid != 0x1532 || f.featureLen != 91) continue;
        const DWORD key = (DWORD(f.vid) << 16) | f.pid;
        if (done[key]) continue;
        HANDLE h = OpenQuery(f.path);
        if (h == INVALID_HANDLE_VALUE) continue;
        BYTE r[91]{};
        BYTE tid = 0;
        for (BYTE t : {BYTE(0x1F), BYTE(0x3F), BYTE(0xFF)}) if (RazerQuery(h, t, 0x07, 0x80, r)) { tid = t; break; }
        if (tid) {
            const int pct = (r[10] * 100 + 127) / 255;
            BYTE c[91]{};
            const bool charging = RazerQuery(h, tid, 0x07, 0x84, c) && c[10] == 1;
            // 0 with the mouse asleep or off: the dongle answers for it; a mouse at 0 % could not answer at all
            if (pct > 0 || charging) {
                std::wstring name = CleanName(f.product.empty() ? L"Razer device" : f.product);
                out.push_back({name, pct, KindOf(all, f.vid, f.pid, name), charging});
                g_razerLast[key] = {out.back(), GetTickCount64()};
                done[key] = true;
            }
        }
        CloseHandle(h);
    }
    for (const auto& [key, last] : g_razerLast) {  // asleep now: its dongle is still plugged in (listed above)
        if (done[key] || GetTickCount64() - last.tick > kRazerKeep) continue;
        const bool present = std::any_of(all.begin(), all.end(), [&](const Interface& f) { return ((DWORD(f.vid) << 16) | f.pid) == key; });
        if (present) { out.push_back(last.battery); out.back().charging = false; done[key] = true; }
    }
}

// ---- Logitech HID++ 2.0 -------------------------------------------------------------------------------------------
constexpr BYTE kSwid = 0x0A;  // our software id: replies carry it back

struct HidppPort {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    USHORT len = 20;
    bool Open(const Interface& f) {
        h = CreateFileW(f.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        len = (std::max<USHORT>)(20, (std::max)(f.inputLen, f.outputLen));
        return h != INVALID_HANDLE_VALUE && ev;
    }
    ~HidppPort() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); if (ev) CloseHandle(ev); }
    bool Io(bool write, BYTE* b, DWORD n, DWORD ms) {
        OVERLAPPED o{}; o.hEvent = ev; ResetEvent(ev);
        DWORD done = 0;
        const BOOL ok = write ? WriteFile(h, b, n, nullptr, &o) : ReadFile(h, b, n, nullptr, &o);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(ev, ms) != WAIT_OBJECT_0) { CancelIoEx(h, &o); GetOverlappedResult(h, &o, &done, TRUE); return false; }
        return GetOverlappedResult(h, &o, &done, FALSE) && done > 0;
    }
    // One request; out receives the 16 parameter bytes of the matching reply.
    bool Call(BYTE slot, BYTE feature, BYTE fn, const BYTE* params, int count, BYTE out[16]) {
        std::vector<BYTE> req(len, 0);
        req[0] = 0x11; req[1] = slot; req[2] = feature; req[3] = static_cast<BYTE>((fn << 4) | kSwid);
        for (int i = 0; i < count && i < 16; ++i) req[4 + i] = params[i];
        if (!Io(true, req.data(), len, 300)) return false;
        const ULONGLONG until = GetTickCount64() + 350;
        std::vector<BYTE> r(len, 0);
        while (GetTickCount64() < until) {
            if (!Io(false, r.data(), len, static_cast<DWORD>(until - GetTickCount64()) + 1)) return false;
            if (r[0] != 0x11 || r[1] != slot) continue;  // other devices, notifications
            if (r[2] == 0xFF && r[3] == feature && r[4] == req[3]) return false;  // HID++ 2.0 error
            if (r[2] == feature && r[3] == req[3]) { memcpy(out, r.data() + 4, 16); return true; }
        }
        return false;
    }
    BYTE FeatureIndex(BYTE slot, USHORT id) {
        const BYTE p[2]{static_cast<BYTE>(id >> 8), static_cast<BYTE>(id & 0xFF)};
        BYTE o[16]{};
        return Call(slot, 0x00, 0, p, 2, o) ? o[0] : 0;
    }
};

struct LogiInfo { std::wstring name; DeviceKind kind = DeviceKind::Other; };
std::map<std::wstring, LogiInfo> g_logiNames;  // "path|slot": names and types never change; only the worker thread reads

bool LogiBattery(HidppPort& port, BYTE slot, int& percent, bool& charging) {
    BYTE o[16]{};
    if (BYTE i = port.FeatureIndex(slot, 0x1004); i && port.Call(slot, i, 1, nullptr, 0, o)) {  // unified battery
        percent = o[0]; charging = o[2] == 1 || o[2] == 2;
        if (percent == 0 && o[1]) percent = o[1] & 8 ? 90 : o[1] & 4 ? 50 : o[1] & 2 ? 20 : 5;  // level flags only
        return true;
    }
    if (BYTE i = port.FeatureIndex(slot, 0x1000); i && port.Call(slot, i, 0, nullptr, 0, o)) {  // battery status
        percent = o[0]; charging = o[2] == 1 || o[2] == 2 || o[2] == 4;
        return true;
    }
    if (BYTE i = port.FeatureIndex(slot, 0x1001); i && port.Call(slot, i, 0, nullptr, 0, o)) {  // voltage (mV)
        const int mv = (o[0] << 8) | o[1];
        percent = std::clamp((mv - 3500) * 100 / (4150 - 3500), 0, 100);
        charging = (o[2] & 0x80) != 0;
        return true;
    }
    return false;
}

LogiInfo LogiName(HidppPort& port, BYTE slot) {
    LogiInfo info;
    BYTE i = port.FeatureIndex(slot, 0x0005);
    BYTE o[16]{};
    if (!i || !port.Call(slot, i, 0, nullptr, 0, o)) return info;
    const int length = (std::min)(static_cast<int>(o[0]), 64);
    std::string name;
    while (static_cast<int>(name.size()) < length) {
        const BYTE at = static_cast<BYTE>(name.size());
        if (!port.Call(slot, i, 1, &at, 1, o)) break;
        for (int k = 0; k < 16 && static_cast<int>(name.size()) < length; ++k) { if (!o[k]) { name.resize(length); break; } name.push_back(static_cast<char>(o[k])); }
    }
    while (!name.empty() && (name.back() == '\0' || name.back() == ' ')) name.pop_back();
    info.name.assign(name.begin(), name.end());
    if (port.Call(slot, i, 2, nullptr, 0, o)) {
        switch (o[0]) {  // device type
        case 0: case 2: info.kind = DeviceKind::Keyboard; break;
        case 3: case 4: case 5: info.kind = DeviceKind::Mouse; break;
        case 8: info.kind = DeviceKind::Headphones; break;
        case 11: info.kind = DeviceKind::Gamepad; break;
        default: break;
        }
    }
    return info;
}

void ReadLogitech(const std::vector<Interface>& all, std::vector<DeviceBattery>& out) {
    for (const auto& f : all) {
        if (f.vid != 0x046D || f.usagePage != 0xFF00 || f.usage != 0x0002) continue;  // the long-report collection
        HidppPort port;
        if (!port.Open(f)) continue;
        int found = 0;
        auto probe = [&](BYTE slot) {
            int percent = -1; bool charging = false;
            if (!LogiBattery(port, slot, percent, charging)) return;
            const std::wstring key = f.path + L"|" + std::to_wstring(slot);
            auto it = g_logiNames.find(key);
            if (it == g_logiNames.end() || it->second.name.empty()) it = g_logiNames.insert_or_assign(key, LogiName(port, slot)).first;
            LogiInfo info = it->second;
            if (info.name.empty()) info.name = CleanName(f.product.empty() ? L"Logitech device" : f.product);
            if (info.kind == DeviceKind::Other) info.kind = GuessKind(info.name, L"");
            out.push_back({info.name, std::clamp(percent, 0, 100), info.kind, charging});
            ++found;
        };
        for (BYTE slot = 1; slot <= 6; ++slot) probe(slot);  // receiver slots (a slot without a device answers an error)
        if (!found) probe(0xFF);                              // a device on its own cable or Bluetooth-less USB
    }
}

// ---- Standard HID Battery Strength (usage page 0x06, usage 0x20) in a feature report --------------------------------
void ReadStandard(const std::vector<Interface>& all, std::vector<DeviceBattery>& out) {
    for (const auto& f : all) {
        if (f.vid == 0x1532 || f.vid == 0x046D || !f.featureLen) continue;  // handled above
        if (Lower(f.path).find(L"{00001124-") != std::wstring::npos) continue;  // Bluetooth HID: Windows reports it already
        HANDLE h = OpenQuery(f.path);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{}; HidP_GetCaps(pp, &caps);
            USHORT count = caps.NumberFeatureValueCaps;
            std::vector<HIDP_VALUE_CAPS> values(count);
            if (count && HidP_GetValueCaps(HidP_Feature, values.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
                for (USHORT k = 0; k < count; ++k) {
                    const auto& v = values[k];
                    const USAGE usage = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                    if (v.UsagePage != 0x06 || usage != 0x20) continue;
                    std::vector<BYTE> report(caps.FeatureReportByteLength, 0);
                    report[0] = v.ReportID;
                    ULONG value = 0;
                    if (HidD_GetFeature(h, report.data(), static_cast<ULONG>(report.size())) &&
                        HidP_GetUsageValue(HidP_Feature, 0x06, 0, 0x20, &value, pp, reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size())) == HIDP_STATUS_SUCCESS) {
                        const LONG lo = v.LogicalMin, hi = v.LogicalMax > v.LogicalMin ? v.LogicalMax : 100;
                        const int pct = std::clamp(static_cast<int>((static_cast<LONG>(value) - lo) * 100 / (hi - lo)), 0, 100);
                        const std::wstring name = CleanName(f.product.empty() ? L"Accessory" : f.product);
                        if (std::none_of(out.begin(), out.end(), [&](const DeviceBattery& b) { return b.name == name; }))
                            out.push_back({name, pct, KindOf(all, f.vid, f.pid, name), false});
                    }
                    break;
                }
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
}

}  // namespace

std::vector<DeviceBattery> ReadHidBatteries() {
    std::vector<DeviceBattery> out;
    const auto all = Enumerate();
    ReadRazer(all, out);
    ReadLogitech(all, out);
    ReadStandard(all, out);
    return out;
}

}  // namespace docky::desk
