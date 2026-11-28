#pragma once
// Menu bar sensors (Settings > Menu Bar: network speed, temperature). Everything here works without administrator
// rights and without a kernel driver:
// - Network: GetIfTable2 octet counters, summed over connected hardware interfaces (filter and loopback interfaces
//   excluded so traffic is not counted twice).
// - CPU line: the "Thermal Zone Information" performance counter (ACPI thermal zones; the hottest valid zone). On most
//   laptops it follows the CPU / embedded controller; some desktops report a fixed value or no zone at all.
// - GPU line: D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERPERFDATA), the source of Task Manager's GPU temperature
//   (WDDM 2.4+ drivers; integrated GPUs usually report 0 = unknown).
// A value that cannot be read is NaN; the menu bar shows "--". MyDockFinder's Temperature.exe instead embeds
// OpenHardwareMonitor with the WinRing0 driver and asks for elevation; that driver is on Microsoft's vulnerable-driver
// block list, so it is deliberately not used here.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "gdi32.lib")
namespace docky::sensors {
inline constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();

// "422.9KB/s": 1024-based, one decimal below 100 of a unit, none above (keeps the text short and steady).
inline std::wstring FormatRate(double bytesPerSecond) {
    if (!(bytesPerSecond >= 0)) return L"--";
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    int unit = 0; double v = bytesPerSecond;
    while (unit < 3 && v >= 1000.0) { v /= 1024.0; ++unit; }
    wchar_t text[32]{};
    if (unit == 0) _snwprintf_s(text, _TRUNCATE, L"%.0f%ls", v, units[unit]);
    else if (v < 100.0) _snwprintf_s(text, _TRUNCATE, L"%.1f%ls", v, units[unit]);
    else _snwprintf_s(text, _TRUNCATE, L"%.0f%ls", v, units[unit]);
    return text;
}
inline std::wstring FormatCelsius(double c) {
    if (!std::isfinite(c)) return L"--°";
    wchar_t text[16]{}; _snwprintf_s(text, _TRUNCATE, L"%.0f°", c); return text;
}

class NetSpeed {
public:
    // Call about once a second. Returns false until two samples exist.
    bool Sample() {
        MIB_IF_TABLE2* table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || !table) return false;
        std::uint64_t in = 0, out = 0;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& r = table->Table[i];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.InterfaceAndOperStatusFlags.FilterInterface ||
                !r.InterfaceAndOperStatusFlags.HardwareInterface || r.OperStatus != IfOperStatusUp) continue;
            in += r.InOctets; out += r.OutOctets;
        }
        FreeMibTable(table);
        const ULONGLONG now = GetTickCount64();
        bool ready = false;
        if (last_ && now > last_) {
            const double dt = static_cast<double>(now - last_) / 1000.0;
            // Counters can restart (adapter reset, interface gone): treat a step backwards as no traffic.
            down_ = in >= lastIn_ ? static_cast<double>(in - lastIn_) / dt : 0.0;
            up_ = out >= lastOut_ ? static_cast<double>(out - lastOut_) / dt : 0.0;
            ready = true;
        }
        last_ = now; lastIn_ = in; lastOut_ = out; valid_ = valid_ || ready;
        return ready;
    }
    bool Valid() const { return valid_; }
    double Down() const { return valid_ ? down_ : kUnknown; }
    double Up() const { return valid_ ? up_ : kUnknown; }
    void Reset() { last_ = 0; valid_ = false; }
private:
    ULONGLONG last_ = 0; std::uint64_t lastIn_ = 0, lastOut_ = 0; double down_ = 0, up_ = 0; bool valid_ = false;
};

class Temperatures {
public:
    ~Temperatures() { Close(); }
    void Close() { if (query_) PdhCloseQuery(query_); query_ = nullptr; counter_ = nullptr; deciKelvin_ = false; tried_ = false; }
    // Call about once a second.
    void Sample() {
        cpu_ = ReadThermalZones();
        const ULONGLONG now = GetTickCount64();
        if (!gpuAt_ || now - gpuAt_ >= 2000) { gpuAt_ = now; gpu_ = ReadGpu(); }  // every 2 s is plenty
    }
    double Cpu() const { return cpu_; }
    double Gpu() const { return gpu_; }
    static bool Plausible(double c) { return std::isfinite(c) && c >= 5.0 && c <= 125.0; }
private:
    double ReadThermalZones() {
        if (!query_ && !tried_) {
            tried_ = true;
            if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) { query_ = nullptr; return kUnknown; }
            // High Precision Temperature is in tenths of a kelvin (Windows 10 1809+); plain Temperature in kelvin.
            if (PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &counter_) == ERROR_SUCCESS) deciKelvin_ = true;
            else if (PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\Temperature", 0, &counter_) != ERROR_SUCCESS) { PdhCloseQuery(query_); query_ = nullptr; counter_ = nullptr; return kUnknown; }
        }
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return kUnknown;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA || !bytes) return kUnknown;
        std::vector<unsigned char> buffer(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return kUnknown;
        double best = kUnknown;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const double c = (deciKelvin_ ? items[i].FmtValue.doubleValue / 10.0 : items[i].FmtValue.doubleValue) - 273.15;
            if (Plausible(c) && !(c <= best)) best = c;
        }
        return best;
    }
    static double ReadGpu() {
        D3DKMT_ENUMADAPTERS2 e{};
        if (D3DKMTEnumAdapters2(&e) != 0 || !e.NumAdapters) return kUnknown;
        std::vector<D3DKMT_ADAPTERINFO> list(e.NumAdapters); e.pAdapters = list.data();
        if (D3DKMTEnumAdapters2(&e) != 0) return kUnknown;
        double best = kUnknown;
        for (ULONG i = 0; i < e.NumAdapters; ++i) {
            D3DKMT_ADAPTER_PERFDATA perf{}; perf.PhysicalAdapterIndex = 0;
            D3DKMT_QUERYADAPTERINFO q{}; q.hAdapter = list[i].hAdapter; q.Type = KMTQAITYPE_ADAPTERPERFDATA;
            q.pPrivateDriverData = &perf; q.PrivateDriverDataSize = sizeof(perf);
            if (D3DKMTQueryAdapterInfo(&q) == 0 && perf.Temperature > 0) {
                const double c = perf.Temperature / 10.0;
                if (Plausible(c) && !(c <= best)) best = c;
            }
            D3DKMT_CLOSEADAPTER close{}; close.hAdapter = list[i].hAdapter; D3DKMTCloseAdapter(&close);
        }
        return best;
    }
    PDH_HQUERY query_ = nullptr; PDH_HCOUNTER counter_ = nullptr; bool deciKelvin_ = false, tried_ = false;
    double cpu_ = kUnknown, gpu_ = kUnknown; ULONGLONG gpuAt_ = 0;
};
}
