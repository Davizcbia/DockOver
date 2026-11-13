#pragma once
#include "custom_icons.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace docky::usersound {
struct Entry { const char* key; const wchar_t* file; };
inline constexpr Entry entries[]={{"sound.muted",L"muted.png"},{"sound.low",L"low.png"},
                                  {"sound.medium",L"medium.png"},{"sound.high",L"high.png"}};
inline bool Load(const wchar_t* name,std::vector<std::uint32_t>& pixels,UINT& width,UINT& height){
 wchar_t module[32768]{};const DWORD n=GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)));
 if(!n||n>=std::size(module))return false;
 std::wstring path(module,n);const auto slash=path.find_last_of(L"\\/");if(slash==std::wstring::npos)return false;
 path.resize(slash);path+=L"\\assets\\user-sound\\";path+=name;std::wstring why;
 if(!dock::LoadCustomIconPixels(path,pixels,width,height,why))return false;
 // The supplied transparent PNGs have low-alpha debris around the artwork.
 // Remove that debris and retain the antialiased edge of the original shape.
 for(auto& p:pixels){const unsigned a=p>>24;if(a<=200){p=0;continue;}const unsigned out=(a-200)*255/55;
  unsigned value=0;for(int c=0;c<3;++c)value|=(std::min)(out,(((p>>(8*c))&255u)*out+a/2)/a)<<(8*c);
  p=value|(out<<24);
 }
 return true;
}
}
