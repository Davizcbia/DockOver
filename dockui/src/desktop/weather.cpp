#include "weather.h"
#include "canvas.h"
#include "../json_lite.h"
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace docky::desk {
using dock::json::Value;

Sky SkyOf(int c) {
    if (c == 0) return Sky::Clear;
    if (c == 1 || c == 2) return Sky::PartlyCloudy;
    if (c == 3) return Sky::Cloudy;
    if (c == 45 || c == 48) return Sky::Fog;
    if (c >= 51 && c <= 57) return Sky::Drizzle;
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return Sky::Rain;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return Sky::Snow;
    if (c >= 95) return Sky::Thunder;
    return Sky::Cloudy;
}
const wchar_t* ConditionText(int c, bool day) {
    switch (c) {
    case 0: return day ? L"Sunny" : L"Clear";
    case 1: return day ? L"Mostly Sunny" : L"Mostly Clear";
    case 2: return L"Partly Cloudy";
    case 3: return L"Cloudy";
    case 45: case 48: return L"Foggy";
    case 51: case 53: case 55: return L"Drizzle";
    case 56: case 57: return L"Freezing Drizzle";
    case 61: return L"Light Rain";
    case 63: return L"Rain";
    case 65: return L"Heavy Rain";
    case 66: case 67: return L"Freezing Rain";
    case 71: case 73: case 75: return L"Snow";
    case 77: return L"Snow Grains";
    case 80: case 81: case 82: return L"Showers";
    case 85: case 86: return L"Snow Showers";
    default: return c >= 95 ? L"Thunderstorms" : L"Cloudy";
    }
}

namespace {
// GET over HTTPS with WinHTTP; returns the body (UTF-8) or "" on any failure.
std::string HttpGet(const std::wstring& url) {
    URL_COMPONENTS u{}; u.dwStructSize = sizeof(u);
    wchar_t host[256]{}, path[2048]{};
    u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host); u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048]{}; u.lpszExtraInfo = extra; u.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return {};
    std::string body;
    HINTERNET session = WinHttpOpen(L"docky-desktop/0.1 (weather widget)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, host, u.nPort, 0);
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail && body.size() < 2u * 1024 * 1024) {
                std::string chunk(avail, '\0'); DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read) break;
                body.append(chunk.data(), read);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}
std::wstring Encode(const std::wstring& s) {
    const std::string u = dock::json::ToUtf8(s); std::wstring out;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out += static_cast<wchar_t>(c);
        else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); out += b; }
    }
    return out;
}
double Num(const Value* v, double fallback = 0) { return v && v->type == Value::Type::Number ? v->number : fallback; }
const Value* Arr(const Value* o, const char* k) { const Value* v = o ? o->Find(k) : nullptr; return v && v->type == Value::Type::Array ? v : nullptr; }
std::wstring Str(const Value* o, const char* k) { const Value* v = o ? o->Find(k) : nullptr; return v && v->type == Value::Type::String ? dock::json::FromUtf8(v->string) : L""; }

struct Shared {
    mutable std::mutex mutex; WeatherData data;
    HWND notify = nullptr; UINT message = 0; HANDLE wake = nullptr; std::atomic<bool> stop{false}; std::thread thread;
    std::atomic<bool> fahrenheit{false};
    double lastLat = 1e9, lastLon = 1e9; std::wstring lastPlace;
} g;

bool AutomaticLocation(double& lat, double& lon, std::wstring& error) {
    try {
        using namespace winrt::Windows::Devices::Geolocation;
        if (Geolocator::RequestAccessAsync().get() != GeolocationAccessStatus::Allowed) { error = L"Location is off"; return false; }
        Geolocator locator; locator.DesiredAccuracy(PositionAccuracy::Default);
        const auto pos = locator.GetGeopositionAsync().get().Coordinate().Point().Position();
        lat = pos.Latitude; lon = pos.Longitude; return true;
    } catch (...) { error = L"Location unavailable"; return false; }
}

void Fetch() {
    WeatherData d; d.fahrenheit = g.fahrenheit;
    double lat = 0, lon = 0;
    std::wstring city;
    {   // first line that is neither empty nor a # comment
        std::ifstream f(Weather::CityPath(), std::ios::binary); std::string line;
        while (std::getline(f, line)) {
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            while (!line.empty() && line.front() == ' ') line.erase(0, 1);
            if (!line.empty() && line[0] != '#') { city = dock::json::FromUtf8(line); break; }
        }
    }
    if (!city.empty()) {
        Value r; std::string err;
        if (!dock::json::Parse(HttpGet(L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=" + Encode(city)), r, err)) d.error = L"Weather service unreachable";
        else if (const Value* res = Arr(&r, "results"); res && !res->array.empty()) {
            const Value& first = res->array[0]; lat = Num(first.Find("latitude")); lon = Num(first.Find("longitude")); d.place = Str(&first, "name");
        } else d.error = L"City not found";
    } else if (AutomaticLocation(lat, lon, d.error)) {
        d.automatic = true;
        lat = std::round(lat * 100) / 100; lon = std::round(lon * 100) / 100;  // ~1 km is plenty for weather
        if (std::fabs(lat - g.lastLat) > 0.02 || std::fabs(lon - g.lastLon) > 0.02 || g.lastPlace.empty()) {
            wchar_t q[160]; swprintf_s(q, L"https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10&accept-language=en&lat=%.2f&lon=%.2f", lat, lon);
            Value r; std::string err;
            if (dock::json::Parse(HttpGet(q), r, err) && r.IsObject()) {
                const Value* a = r.Find("address");
                for (const char* k : {"city", "town", "village", "hamlet", "municipality", "county"}) { const auto v = Str(a, k); if (!v.empty()) { g.lastPlace = v; break; } }
            }
            g.lastLat = lat; g.lastLon = lon;
        }
        d.place = g.lastPlace.empty() ? L"Current Location" : g.lastPlace;
    }
    if (d.error.empty()) {
        wchar_t q[600];
        swprintf_s(q, L"https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code,is_day"
                      L"&hourly=temperature_2m,weather_code,is_day&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=6&temperature_unit=%ls",
                   lat, lon, d.fahrenheit ? L"fahrenheit" : L"celsius");
        Value r; std::string err;
        if (!dock::json::Parse(HttpGet(q), r, err) || !r.IsObject()) d.error = L"Weather service unreachable";
        else {
            const Value* cur = r.Find("current");
            d.temp = static_cast<float>(Num(cur ? cur->Find("temperature_2m") : nullptr));
            d.code = static_cast<int>(Num(cur ? cur->Find("weather_code") : nullptr));
            d.day = Num(cur ? cur->Find("is_day") : nullptr, 1) != 0;
            const std::wstring now = Str(cur, "time");  // "2026-11-11T20:15"
            const Value* hourly = r.Find("hourly");
            const Value* ht = Arr(hourly, "time"); const Value* htemp = Arr(hourly, "temperature_2m"); const Value* hcode = Arr(hourly, "weather_code"); const Value* hday = Arr(hourly, "is_day");
            if (ht && htemp && hcode) {
                size_t start = 0;
                for (size_t i = 0; i < ht->array.size(); ++i) if (dock::json::FromUtf8(ht->array[i].string).substr(0, 13) == now.substr(0, 13)) { start = i; break; }
                for (size_t i = start; i < ht->array.size() && d.hours.size() < 6; ++i) {
                    WeatherHour h; const std::wstring t = dock::json::FromUtf8(ht->array[i].string);
                    h.hour = t.size() >= 13 ? _wtoi(t.substr(11, 2).c_str()) : 0;
                    h.temp = static_cast<float>(Num(&htemp->array[i])); h.code = static_cast<int>(Num(i < hcode->array.size() ? &hcode->array[i] : nullptr));
                    h.day = !hday || i >= hday->array.size() || Num(&hday->array[i], 1) != 0;
                    d.hours.push_back(h);
                }
            }
            const Value* daily = r.Find("daily");
            const Value* dt = Arr(daily, "time"); const Value* dmax = Arr(daily, "temperature_2m_max"); const Value* dmin = Arr(daily, "temperature_2m_min"); const Value* dcode = Arr(daily, "weather_code");
            if (dt && dmax && dmin && dcode) {
                for (size_t i = 0; i < dt->array.size() && i < dmax->array.size() && i < dmin->array.size() && i < dcode->array.size(); ++i) {
                    WeatherDay day; day.high = static_cast<float>(Num(&dmax->array[i])); day.low = static_cast<float>(Num(&dmin->array[i])); day.code = static_cast<int>(Num(&dcode->array[i]));
                    const std::wstring t = dock::json::FromUtf8(dt->array[i].string);
                    SYSTEMTIME st{}; st.wYear = static_cast<WORD>(_wtoi(t.substr(0, 4).c_str())); st.wMonth = static_cast<WORD>(_wtoi(t.substr(5, 2).c_str())); st.wDay = static_cast<WORD>(_wtoi(t.substr(8, 2).c_str()));
                    FILETIME ft{}; SystemTimeToFileTime(&st, &ft); FileTimeToSystemTime(&ft, &st);
                    static const wchar_t* names[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
                    day.name = i == 0 ? L"Today" : names[st.wDayOfWeek % 7];
                    d.days.push_back(day);
                }
                if (!d.days.empty()) {
                    d.high = d.days[0].high; d.low = d.days[0].low;
                    d.weekLow = d.days[0].low; d.weekHigh = d.days[0].high;
                    for (const auto& x : d.days) { d.weekLow = (std::min)(d.weekLow, x.low); d.weekHigh = (std::max)(d.weekHigh, x.high); }
                }
            }
            d.valid = true;
        }
    }
    { std::lock_guard<std::mutex> l(g.mutex); if (d.valid || !g.data.valid) g.data = d; else g.data.error = d.error; }
    if (g.notify) PostMessageW(g.notify, g.message, 0, 0);
}
}  // namespace

std::wstring Weather::CityPath() { return DataDir() + L"\\weather-city.txt"; }

void Weather::Start(HWND notify, UINT message) {
    if (g.thread.joinable()) return;
    g.notify = notify; g.message = message; g.stop = false;
    g.wake = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    if (GetFileAttributesW(CityPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(DataDir().c_str(), nullptr);
        std::ofstream f(CityPath(), std::ios::binary);
        f << "# Type a city on its own line below (for example: Storrs) to use it instead of Windows Location.\r\n";
    }
    g.thread = std::thread([] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (!g.stop) {
            if (WaitForSingleObject(g.wake, 15 * 60 * 1000) == WAIT_FAILED || g.stop) break;
            Fetch();
        }
        winrt::uninit_apartment();
    });
}
void Weather::Stop() { g.stop = true; if (g.wake) SetEvent(g.wake); if (g.thread.joinable()) g.thread.join(); if (g.wake) { CloseHandle(g.wake); g.wake = nullptr; } }
void Weather::Refresh() { if (g.wake) SetEvent(g.wake); }
WeatherData Weather::Data() const { std::lock_guard<std::mutex> l(g.mutex); return g.data; }
void Weather::SetFahrenheit(bool f) { if (g.fahrenheit.exchange(f) != f) Refresh(); }
bool Weather::Fahrenheit() const { return g.fahrenheit; }
Weather& WeatherService() { static Weather w; return w; }

}  // namespace docky::desk
