#include "desktop_settings.h"
#include "settings_search.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <fstream>
#include <iterator>
using namespace liquid::settings;
int wmain(int argc, wchar_t** argv) {
    if(SearchCategories(L"").size()!=10 || SearchCategories(L"   ").size()!=10 ||
       SearchCategories(L"  DOCK   hide ")!=std::vector<int>{2} ||
       SearchCategories(L"visual effects")!=std::vector<int>{0} ||
       SearchCategories(L"windows taskbar")!=std::vector<int>{6} ||
       SearchCategories(L"density")!=std::vector<int>{4} ||
       SearchCategories(L"legacy backup")!=std::vector<int>{8} || SearchCategories(L"about")!=std::vector<int>{9} ||
       SearchCategories(L"no-such-setting").size()!=0)return 20;
    if(argc==3 && std::wstring(argv[1])==L"--density-writer"){
        for(int i=0;i<40;++i)if(WriteDensityInt(argv[2],"iconSize",32+(i%17)*8)!=SaveStatus::Saved)return 11;
        return 0;
    }
    if(argc==4 && std::wstring(argv[1])==L"--writer"){
        std::string prefix=argv[3][0]==L'A'?"a":"b";
        for(int i=0;i<40;++i)if(WriteBool(argv[2],"appearance",prefix+std::to_string(i),true)!=SaveStatus::Saved)return 10;
        return 0;
    }
    wchar_t dir[MAX_PATH]{}, file[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"lds",0,file)) return 1;
    auto put=[&](const char* text){
        HANDLE h=CreateFileW(file,GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false; DWORD n=0; auto len=static_cast<DWORD>(strlen(text));
        bool ok=WriteFile(h,text,len,&n,nullptr)&&n==len;CloseHandle(h);return ok;
    };
    auto check=[&](const char* text,LoadStatus expected,bool diag){
        if(!put(text))return false;
        auto a=Load(file); auto b=Load(file);
        std::ifstream in(file,std::ios::binary);
        std::string after((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        return after==text&&a.status==expected&&b.status==expected&&a.value.dock.diagnostics==diag&&b.value.dock.diagnostics==diag;
    };
    bool ok=check("{\"schemaVersion\":1}",LoadStatus::Loaded,false)
      &&check("{\"schemaVersion\":1,\"dock\":{\"diagnostics\":true},\"menuBar\":{},\"launchpad\":{},\"controlCenter\":{},\"appearance\":{},\"taskbar\":{}}",LoadStatus::Loaded,true)
      &&check("{bad",LoadStatus::Invalid,false)
      &&check("{\"schemaVersion\":2}",LoadStatus::Invalid,false)
      &&check("{\"schemaVersion\":1,\"dock\":{\"diagnostics\":\"true\"}}",LoadStatus::Invalid,false)
      &&check("{\"schemaVersion\":1,\"taskbar\":false}",LoadStatus::Invalid,false);
    ok=ok&&Load(file).value.dock.useMacIconMask;
    auto read=[&](){std::ifstream in(file,std::ios::binary);return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());};
    const char* original="{\"schemaVersion\":1,\"unknown\":{\"large\":9007199254740993,\"raw\":\"\\u0061\"},\"dock\":{\"extra\":[1,2]}}   ";
    ok=ok&&put(original);
    const auto initialRevision=ReadRevision(file);
    ok=ok&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Saved;
    const auto changedRevision=ReadRevision(file);
    ok=ok&&initialRevision!=changedRevision&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Saved&&ReadRevision(file)==changedRevision;
    auto written=read();ok=ok&&written.find("\"large\":9007199254740993")!=std::string::npos&&written.find("\"raw\":\"\\u0061\"")!=std::string::npos&&Load(file).value.dock.diagnostics;
    ok=ok&&Load(file).value.appearance.mode==0;
    for(int mode=0;mode<3;++mode)ok=ok&&WriteAppearanceMode(file,mode)==SaveStatus::Saved&&Load(file).value.appearance.mode==mode;
    {auto saved=read();ok=ok&&WriteAppearanceMode(file,3)==SaveStatus::Invalid&&read()==saved&&saved.find("\"large\":9007199254740993")!=std::string::npos;}
    ok=ok&&WriteDensityInt(file,"densityPreset",3)==SaveStatus::Saved&&WriteDensityInt(file,"iconSize",120)==SaveStatus::Saved;
    ok=ok&&WriteDensityInt(file,"gapX",60)==SaveStatus::Saved&&WriteDensityInt(file,"gapY",40)==SaveStatus::Saved;
    {auto value=Load(file);ok=ok&&value.status==LoadStatus::Loaded&&value.value.launchpad.densityPreset==3&&value.value.launchpad.iconSize==120&&value.value.launchpad.gapX==60&&value.value.launchpad.gapY==40;}
    written=read();ok=ok&&written.find("\"large\":9007199254740993")!=std::string::npos;
    {auto saved=read();ok=ok&&WriteDensityInt(file,"iconSize",161)==SaveStatus::Invalid&&WriteDensityInt(file,"unknown",1)==SaveStatus::Invalid&&read()==saved;}
    ok=ok&&WriteBool(file,"menuBar","enabled",true)==SaveStatus::Saved&&Load(file).status==LoadStatus::Loaded;
    ok=ok&&WriteBool(file,"dock","useMacIconMask",false)==SaveStatus::Saved&&!Load(file).value.dock.useMacIconMask;
    ok=ok&&put("{\"schemaVersion\":1,\"launchpad\":{\"iconSize\":\"wrong\"}}")&&Load(file).status==LoadStatus::Invalid;
    {auto saved=read();ok=ok&&WriteDensityInt(file,"iconSize",96)==SaveStatus::Invalid&&read()==saved;}
    ok=ok&&put("{broken")&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Invalid&&read()=="{broken";
    ok=ok&&put("{\"schemaVersion\":2}")&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Invalid&&read()=="{\"schemaVersion\":2}";
    ok=ok&&put("{\"schemaVersion\":1,\"dock\":{\"diagnostics\":false}}");
    HANDLE deny=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    auto before=read();auto failed=WriteBool(file,"dock","diagnostics",true);
    if(deny!=INVALID_HANDLE_VALUE)CloseHandle(deny);
    ok=ok&&deny!=INVALID_HANDLE_VALUE&&failed==SaveStatus::Unavailable&&read()==before;
    wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);
    auto spawn=[&](wchar_t suffix,PROCESS_INFORMATION& pi){
        std::wstring cmd=L"\""+std::wstring(self)+L"\" --writer \""+std::wstring(file)+L"\" "+suffix;
        STARTUPINFOW si{};si.cb=sizeof(si);return CreateProcessW(self,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=FALSE;
    };
    PROCESS_INFORMATION a{},b{},densityProcess{};bool pa=spawn(L'A',a),pb=spawn(L'B',b);
    std::wstring densityCmd=L"\""+std::wstring(self)+L"\" --density-writer \""+std::wstring(file)+L"\"";
    STARTUPINFOW densityStart{};densityStart.cb=sizeof(densityStart);
    const bool pc=CreateProcessW(self,densityCmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&densityStart,&densityProcess)!=FALSE;
    if(pa){WaitForSingleObject(a.hProcess,30000);DWORD code=99;GetExitCodeProcess(a.hProcess,&code);ok=ok&&code==0;CloseHandle(a.hThread);CloseHandle(a.hProcess);}
    if(pb){WaitForSingleObject(b.hProcess,30000);DWORD code=99;GetExitCodeProcess(b.hProcess,&code);ok=ok&&code==0;CloseHandle(b.hThread);CloseHandle(b.hProcess);}
    if(pc){WaitForSingleObject(densityProcess.hProcess,30000);DWORD code=99;GetExitCodeProcess(densityProcess.hProcess,&code);ok=ok&&code==0;CloseHandle(densityProcess.hThread);CloseHandle(densityProcess.hProcess);}
    written=read();ok=ok&&pa&&pb&&pc&&Load(file).value.launchpad.iconSize>=32&&Load(file).value.launchpad.iconSize<=160;
    for(int i=0;i<40;++i)ok=ok&&written.find("\"a"+std::to_string(i)+"\"")!=std::string::npos&&written.find("\"b"+std::to_string(i)+"\"")!=std::string::npos;
    const auto backup=std::wstring(file)+L".liquid-desktop-v02.bak";
    const auto sourceBefore=read();
    const auto firstBackup=BackupLegacy(file);const auto secondBackup=BackupLegacy(file);
    std::ifstream backupIn(backup,std::ios::binary);
    const std::string backupBytes((std::istreambuf_iterator<char>(backupIn)),std::istreambuf_iterator<char>());
    backupIn.close();
    ok=ok&&firstBackup==BackupStatus::Created&&secondBackup==BackupStatus::AlreadyExists&&read()==sourceBefore&&backupBytes==sourceBefore;
    DeleteFileW(backup.c_str());
    DeleteFileW(file);
    ok=ok&&Load(file).status==LoadStatus::Missing;
    ok=ok&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Saved&&Load(file).value.dock.diagnostics;
    DeleteFileW(file);
    std::puts(ok?"desktop settings: all checks passed":"desktop settings: FAILED");return ok?0:1;
}
