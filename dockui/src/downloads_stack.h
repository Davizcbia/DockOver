#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
namespace dock::downloads {
enum class View { Fan, Grid, List };
enum class Sort { Date, Name, Type };
enum class Display { Stack, Folder };
struct Settings { View view=View::Fan; Sort sort=Sort::Date; Display display=Display::Stack; };
struct Entry { std::wstring name,path; DWORD attributes=0; FILETIME modified{}; };
std::wstring KnownDownloadsPath();
std::vector<std::uint32_t> MakeStackIcon(unsigned size);
// Open-state Dock icon (original vector art): translucent graphite rounded square, lighter at the top,
// with a white down arrow. Straight-alpha BGRA like the other Dock icon sources.
std::vector<std::uint32_t> MakeOpenStackIcon(unsigned size);
// 0..1 opening progress of the Fan/Grid/List panel (falls back to 0 when closed).
float OpenProgress();
constexpr unsigned kProgressMessage = WM_APP + 184;  // posted to the owner (Dock) while the progress changes
bool LoadSettings(const std::wstring& path,Settings& result,std::wstring& error);
bool SaveSettings(const std::wstring& path,const Settings& settings,std::wstring& error);
std::vector<Entry> Scan(const std::wstring& directory,DWORD& error,size_t limit=5000);
void SortEntries(std::vector<Entry>& entries,Sort sort);
// Shows an independent read-only panel; the caller remains responsible for its window lifetime.
bool Toggle(HWND owner,const RECT& iconScreen,const std::wstring& directory,const Settings& settings);
void Close();
}
