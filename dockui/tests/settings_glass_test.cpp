#include "glass_params.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cmath>
using namespace dock;
int wmain(){
 wchar_t dir[MAX_PATH]{},path[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"sgl",0,path))return 1;
 {std::ofstream f(path);f<<R"({"format":"LiquidDockGlass","version":1,"current":{},"surfaces":{"controls":{"radius":14}}})";}
 auto read=[&](){std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{});};
 auto& store=GlassSettingsStore::Get();store.Load(path);
 auto search=store.Surface(GlassSurface::SettingsSearch),navigation=store.Surface(GlassSurface::SettingsNavigation);
 search.radius=9;search.refraction=.31f;navigation.radius=23;navigation.refraction=.87f;
 store.SetSurface(GlassSurface::SettingsSearch,search);store.SetSurface(GlassSurface::SettingsNavigation,navigation);
 store.SetUserDefault(GlassSurface::SettingsSearch,search);
 bool ok=store.Save();const auto saved=read();
 ok=ok&&saved.find("settingsSearch")!=std::string::npos&&saved.find("settingsNavigation")!=std::string::npos;
 store.Load(path);
 ok=ok&&std::fabs(store.Surface(GlassSurface::SettingsSearch).refraction-.31f)<.001f&&std::fabs(store.Surface(GlassSurface::SettingsNavigation).refraction-.87f)<.001f;
 ok=ok&&store.Surface(GlassSurface::Controls).radius==14&&store.UserDefault(GlassSurface::SettingsSearch).radius==9;
 store.LoadReadOnly(path);search.radius=3;store.SetSurface(GlassSurface::SettingsSearch,search);
 ok=ok&&!store.Save()&&read()==saved;
 DeleteFileW(path);std::puts(ok?"PASS independent Settings glass surfaces, legacy load, defaults and read-only persistence":"FAIL Settings glass persistence");return ok?0:2;
}
