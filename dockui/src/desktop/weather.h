#pragma once
// Weather for the Weather widget. Location: Windows Location (Settings > Privacy > Location, "Let desktop apps access
// your location") or a city typed into %LOCALAPPDATA%\DockyDesktop\weather-city.txt (which wins when present).
// Forecast: Open-Meteo (open-meteo.com, free, no account or key). City name for an automatic location: OpenStreetMap
// Nominatim reverse geocoding. Only the coordinates (rounded to ~1 km) or the typed city name are sent; refreshed
// every 15 minutes on a worker thread.
#include <windows.h>
#include <string>
#include <vector>

namespace docky::desk {

struct WeatherHour { int hour = 0; float temp = 0; int code = 0; bool day = true; };
struct WeatherDay { std::wstring name; float low = 0, high = 0; int code = 0; };
struct WeatherData {
    bool valid = false, automatic = false, fahrenheit = false;
    std::wstring place, error;
    float temp = 0, high = 0, low = 0;
    int code = 0; bool day = true;
    std::vector<WeatherHour> hours;   // next 6 hours
    std::vector<WeatherDay> days;     // today + 5
    float weekLow = 0, weekHigh = 0;  // range of `days`, for the large widget's bars
};

// WMO weather code -> short condition text (English, as the macOS widget) and an icon kind.
enum class Sky { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Thunder };
Sky SkyOf(int wmoCode);
const wchar_t* ConditionText(int wmoCode, bool day);

class Weather {
public:
    void Start(HWND notify, UINT message);
    void Stop();
    void Refresh();
    WeatherData Data() const;
    void SetFahrenheit(bool f);   // unit preference (default from the Windows measurement system)
    bool Fahrenheit() const;
    static std::wstring CityPath();
};
Weather& WeatherService();

}  // namespace docky::desk
