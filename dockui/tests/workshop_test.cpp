#include "workshop.h"
#include "json_lite.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <string>
#include <vector>
using namespace docky::workshop;
#define CHECK(x) do{if(!(x)){std::fwprintf(stderr,L"FAIL line %d: %ls\n",__LINE__,err.c_str());return 1;}}while(0)
int wmain(int argc,wchar_t** argv){if(argc==4&&std::wstring(argv[1])==L"--combined-pack"){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring err;Pack p,actual;bool enabled=false;const std::wstring root=std::wstring(argv[3])+L"\\workshop";
 CHECK(Install(root,argv[2],p,err));CHECK(p.icons.size()==13&&p.icons.count("dock.finder")&&p.icons.count("dock.finder.dark")&&p.icons.count("dock.launchpad")&&p.icons.count("dock.launchpad.dark")&&p.icons.count("settings.general")&&p.icons.count("system.logo"));
 CHECK(Select(root,p.id,err)&&SelectSettings(root,p.id,err));CHECK(Active(root,actual,enabled,err)&&enabled&&actual.id==p.id&&actual.icons.count("dock.finder.dark"));CHECK(ActiveSettings(root,actual,enabled,err)&&enabled&&actual.id==p.id&&actual.icons.count("settings.general"));
 std::puts("PASS combined pack installation and both selection slots");CoUninitialize();return 0;}
if(argc==5&&std::wstring(argv[1])==L"--dual-packs"){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring err;Pack original,settings,actual;bool enabled=false;const std::wstring root=std::wstring(argv[4])+L"\\workshop";
 CHECK(Install(root,argv[2],original,err));CHECK(Install(root,argv[3],settings,err));CHECK(Select(root,original.id,err));CHECK(SelectSettings(root,settings.id,err));
 CHECK(Active(root,actual,enabled,err)&&enabled&&actual.id==original.id);CHECK(ActiveSettings(root,actual,enabled,err)&&enabled&&actual.id==settings.id);
 CHECK(SelectSettings(root,"",err));CHECK(Active(root,actual,enabled,err)&&enabled&&actual.id==original.id);CHECK(ActiveSettings(root,actual,enabled,err)&&!enabled);
 CHECK(SelectSettings(root,settings.id,err));CHECK(Remove(root,settings.id,err));CHECK(ActiveSettings(root,actual,enabled,err)&&!enabled);CHECK(Active(root,actual,enabled,err)&&enabled&&actual.id==original.id);
 std::puts("PASS independent global and Settings pack selection, disable and removal");CoUninitialize();return 0;}
if(argc==4&&std::wstring(argv[1])==L"--settings-pack"){
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring err;Pack p;bool enabled=false;const std::wstring parent=argv[3],root=parent+L"\\workshop";
 CHECK(Install(root,argv[2],p,err));CHECK(p.id=="settings-original-v1"&&p.icons.size()==10&&p.icons.count("settings.general")&&p.icons.count("settings.about"));
 CHECK(Select(root,p.id,err));CHECK(Active(root,p,enabled,err)&&enabled&&p.icons.size()==10);
 const std::wstring exported=parent+L"\\export.dockypack";CHECK(Export(root,p.id,exported,err));CHECK(Select(root,"",err));CHECK(Active(root,p,enabled,err)&&!enabled);
 CHECK(Remove(root,"settings-original-v1",err));CHECK(Install(root,exported,p,err));CHECK(Select(root,p.id,err));CHECK(Active(root,p,enabled,err)&&enabled&&p.icons.count("settings.general"));
 std::puts("PASS settings icon pack install, selection, disable, export and reimport");CoUninitialize();return 0;}
if(argc==4&&std::wstring(argv[1])==L"--protect"){CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring err;Pack p;bool enabled=false;const std::wstring root=argv[3];
 if(!Install(root,argv[2],p,err)||!Select(root,p.id,err))return 4;const auto note=root+L"\\packs\\reference-menubar-v19\\unowned.txt";HANDLE f=CreateFileW(note.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return 5;DWORD n=0;const char marker[]="preserve";WriteFile(f,marker,sizeof(marker)-1,&n,nullptr);CloseHandle(f);
 if(Remove(root,p.id,err)||GetFileAttributesW(note.c_str())==INVALID_FILE_ATTRIBUTES||!Active(root,p,enabled,err)||!enabled)return 6;std::puts("PASS unexpected file and active selection preserved");CoUninitialize();return 0;}if(argc==4&&(std::wstring(argv[1])==L"--install"||std::wstring(argv[1])==L"--select")){CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring error;Pack p;bool ok=std::wstring(argv[1])==L"--install"?Install(argv[3],argv[2],p,error):Select(argv[2],std::wstring(argv[3])==L"default"?std::string{}:dock::json::ToUtf8(argv[3]),error);if(!ok)std::fwprintf(stderr,L"%ls\n",error.c_str());CoUninitialize();return ok?0:1;}if(argc==4&&std::wstring(argv[1])==L"--reject"){CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring err;Pack p;const bool accepted=Install(argv[3],argv[2],p,err);CoUninitialize();if(accepted){std::fputs("FAIL malicious archive accepted\n",stderr);return 3;}std::puts("PASS malicious archive rejected");return 0;}if(argc!=3)return 2;CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring root=std::wstring(argv[2])+L"\\user\\workshop",err;Pack p;bool enabled=false;std::vector<Pack> list;
 CHECK(Install(root,argv[1],p,err));CHECK(p.id=="reference-menubar-v19"&&p.icons.count("wifi.connected"));
 CHECK(!Install(root,argv[1],p,err));err.clear();CHECK(List(root,list,err)&&list.size()==1);
 CHECK(Select(root,"reference-menubar-v19",err));CHECK(Active(root,p,enabled,err)&&enabled&&p.id=="reference-menubar-v19");
 const std::wstring exported=std::wstring(argv[2])+L"\\export.dockypack";CHECK(Export(root,p.id,exported,err));CHECK(!Export(root,p.id,exported,err));
 CHECK(Select(root,"",err));CHECK(Active(root,p,enabled,err)&&!enabled);
 CHECK(Remove(root,"reference-menubar-v19",err));CHECK(List(root,list,err)&&list.empty());CHECK(Install(root,exported,p,err));
 CHECK(!Select(root,"../bad",err));CHECK(Select(root,"reference-menubar-v19",err));CHECK(Active(root,p,enabled,err)&&enabled);
 HANDLE damaged=CreateFileW(p.icons.at("wifi.connected").c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(damaged!=INVALID_HANDLE_VALUE);DWORD written=0;const char bad[]="damaged";CHECK(WriteFile(damaged,bad,sizeof(bad)-1,&written,nullptr));CloseHandle(damaged);
 enabled=true;CHECK(!Active(root,p,enabled,err)&&!enabled);CHECK(List(root,list,err)&&list.size()==1&&!list[0].valid);CHECK(Select(root,"",err));CHECK(Active(root,p,enabled,err)&&!enabled);CHECK(Remove(root,"reference-menubar-v19",err));
 const auto selection=root+L"\\active.txt";HANDLE state=CreateFileW(selection.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(state!=INVALID_HANDLE_VALUE);const char invalid[]="../invalid";CHECK(WriteFile(state,invalid,sizeof(invalid)-1,&written,nullptr));CloseHandle(state);
 CHECK(!Select(root,"reference-menubar-v19",err));CHECK(Select(root,"",err));WIN32_FIND_DATAW backup{};HANDLE found=FindFirstFileW((root+L"\\active-corrupt-*.bak").c_str(),&backup);CHECK(found!=INVALID_HANDLE_VALUE);FindClose(found);
 std::puts("PASS install, duplicate refusal, list, select, disable, export no-overwrite, remove, reimport, invalid id, damaged-pack fallback, corrupt selection backup");CoUninitialize();return 0;}
