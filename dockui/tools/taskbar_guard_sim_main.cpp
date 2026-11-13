// Only accepts inherited simulation objects; never accepts real taskbar window operations.
#include "taskbar_guard_sim.h"
#include "liquid_build_id.h"
#include <windows.h>
#include <cstdlib>
#include <string>
static volatile const char marker[]=LIQUID_BUILD_MARKER;
int wmain(int argc,wchar_t** argv){(void)marker[0];if(argc!=10||std::wstring(argv[1])!=L"--simulate")return 2;
 HANDLE h[7]{};for(int i=0;i<7;++i){wchar_t* end=nullptr;auto value=_wcstoui64(argv[i+2],&end,10);if(!end||*end||!value)return 2;h[i]=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));}
 return docky::guard_sim::RunGuard(h[0],h[1],h[2],h[3],h[4],h[5],h[6],argv[9]);}
