#include "custom_icons.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(co))return 3;std::vector<uint32_t> pixels;UINT w=0,h=0;std::wstring error;
if(!dock::LoadCustomIconPixels(argv[1],pixels,w,h,error)||w!=512||h!=512||pixels.size()!=512u*512u){std::fwprintf(stderr,L"valid SVG failed: %ls\n",error.c_str());return 4;}
size_t nonzero=0;for(auto px:pixels){unsigned a=px>>24;if(a){++nonzero;if(((px>>16)&255)>a||((px>>8)&255)>a||(px&255)>a)return 5;}}if(nonzero<1000||pixels[0]!=0)return 6;
if(dock::LoadCustomIconPixels(argv[2],pixels,w,h,error))return 7;if(!pixels.empty()||w||h)return 8;
std::puts("PASS SVG valid render, alpha, bounds and malformed rejection");CoUninitialize();return 0;}
