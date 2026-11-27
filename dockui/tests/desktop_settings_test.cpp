#include "desktop_settings.h"
#include "settings_search.h"
#include "settings_motion.h"
#include "game_compat.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cmath>
using namespace liquid::settings;
int wmain(int argc, wchar_t** argv) {
    // Scroll reversals retain position; each viewport is independent and hit coordinates invert drawing.
    {ScrollMotion left,right;left.Bounds(500);right.Bounds(900);left.By(300);
     for(int i=0;i<15;++i)left.Step(1.0/240,false);
     const float before=left.Position();left.By(-200);if(left.Position()!=before||right.Position()!=0)return 21;
     for(int i=0;i<480;++i)left.Step(1.0/240,false);if(std::fabs(left.Position()-100)>0.05f)return 22;
     left.Bounds(40);if(left.Position()>40||left.Target()>40)return 23;
     right.By(800);right.Step(0,true);if(right.Position()!=800)return 24;
     if(SettingsBodyY(100,0)!=100||SettingsBodyY(100,20)!=120)return 27;
     const float y=300,scroll=123;if(std::fabs(SettingsBodyY(y-scroll,scroll)-y)>.001f)return 25;
    }

    if(SearchCategories(L"").size()!=11 || SearchCategories(L"   ").size()!=11 || SearchCategories(L"lyrics")!=std::vector<int>{10} ||
       SearchCategories(L"  DOCK   hide ")!=std::vector<int>{2} ||
       SearchCategories(L"visual effects")!=std::vector<int>{0} ||
       SearchCategories(L"windows taskbar")!=std::vector<int>{6} ||
       SearchCategories(L"density")!=std::vector<int>{4} ||
       SearchCategories(L"menu bar size")!=std::vector<int>{3} ||
       SearchCategories(L"legacy backup")!=std::vector<int>{8} || SearchCategories(L"about")!=std::vector<int>{9} ||
       SearchCategories(L"upd")!=std::vector<int>{9} || SearchCategories(L"check for updates")!=std::vector<int>{9} ||
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
    ok=ok&&put("{\"schemaVersion\":1}")&&Load(file).value.appearance.settingsScalePercent==90&&Load(file).value.appearance.applicationsScalePercent==80;
    ok=ok&&WriteUiScale(file,"settingsScalePercent",75)==SaveStatus::Saved&&WriteUiScale(file,"applicationsScalePercent",120)==SaveStatus::Saved;
    ok=ok&&Load(file).value.appearance.settingsScalePercent==75&&Load(file).value.appearance.applicationsScalePercent==120;
    ok=ok&&WriteUiScale(file,"settingsScalePercent",74)==SaveStatus::Invalid&&WriteUiScale(file,"applicationsScalePercent",121)==SaveStatus::Invalid;
    ok=ok&&Load(file).value.appearance.settingsScalePercent==75&&Load(file).value.appearance.applicationsScalePercent==120;
    for(const auto* bad:{"{\"schemaVersion\":1,\"appearance\":{\"settingsScalePercent\":90.5}}","{\"schemaVersion\":1,\"appearance\":{\"applicationsScalePercent\":\"80\"}}"})ok=ok&&put(bad)&&Load(file).status==LoadStatus::Invalid;
    ok=ok&&put("{\"schemaVersion\":1}")&&Load(file).value.appearance.language=="en";
    ok=ok&&WriteLanguage(file,"en")==SaveStatus::Saved&&Load(file).value.appearance.language=="en";
    ok=ok&&WriteLanguage(file,"zh-CN")==SaveStatus::Saved&&Load(file).value.appearance.language=="zh-CN";
    ok=ok&&WriteLanguage(file,"unsupported")==SaveStatus::Invalid&&Load(file).value.appearance.language=="zh-CN";
    // Legacy default, persisted scale bounds, invalid writes and noninteger/type rejection.
    ok=ok&&put("{\"schemaVersion\":1}")&&Load(file).value.menuBar.scalePercent==100;
    ok=ok&&WriteMenuBarScale(file,80)==SaveStatus::Saved&&Load(file).value.menuBar.scalePercent==80;
    ok=ok&&WriteMenuBarScale(file,160)==SaveStatus::Saved&&Load(file).value.menuBar.scalePercent==160;
    ok=ok&&WriteMenuBarScale(file,79)==SaveStatus::Invalid&&WriteMenuBarScale(file,161)==SaveStatus::Invalid&&Load(file).value.menuBar.scalePercent==160;
    // Menu bar on/off and the Dock status group's cells.
    ok=ok&&Load(file).value.menuBar.enabled&&WriteBool(file,"menuBar","enabled",false)==SaveStatus::Saved&&!Load(file).value.menuBar.enabled;
    ok=ok&&WriteBool(file,"menuBar","enabled",true)==SaveStatus::Saved&&Load(file).value.menuBar.enabled;
    {const auto defaults=Load(file).value.dock.statusCells;ok=ok&&defaults==std::vector<std::wstring>{L"system",L"tray"};}
    ok=ok&&WriteStatusCells(file,{L"wifi",L"bluetooth",L"ext:com.example.vpn"})==SaveStatus::Saved&&
           Load(file).value.dock.statusCells==std::vector<std::wstring>{L"wifi",L"bluetooth",L"ext:com.example.vpn"};
    ok=ok&&WriteStatusCells(file,{L"wifi",L"wifi"})==SaveStatus::Invalid&&WriteStatusCells(file,{L"nope"})==SaveStatus::Invalid&&WriteStatusCells(file,{L"ext:bad id"})==SaveStatus::Invalid;
    // Tray icons kept in the Dock: their tray key (exe name, '#', uid or guid).
    ok=ok&&ValidStatusCell(L"tray:clash-verge.exe#1")&&ValidStatusCell(L"tray:my app (x86).exe#{0B1E1E5C-2D45-4E4B-8B0F-6A1F0C2D3E4F}");
    ok=ok&&!ValidStatusCell(L"tray:")&&!ValidStatusCell(L"tray:#1")&&!ValidStatusCell(L"tray:app.exe")&&!ValidStatusCell(L"tray:app.exe#")&&!ValidStatusCell(L"tray:a\\b.exe#1")&&!ValidStatusCell(L"tray:a\"b.exe#1");
    ok=ok&&WriteStatusCells(file,{L"combo",L"tray:clash-verge.exe#1",L"tray"})==SaveStatus::Saved&&
           Load(file).value.dock.statusCells==std::vector<std::wstring>{L"combo",L"tray:clash-verge.exe#1",L"tray"};
    ok=ok&&WriteStatusCells(file,{})==SaveStatus::Saved&&Load(file).value.dock.statusCells.empty()&&Load(file).status==LoadStatus::Loaded;
    ok=ok&&Load(file).value.appearance.applicationsLaunchpadOnly;
    ok=ok&&WriteBool(file,"appearance","applicationsLaunchpadOnly",false)==SaveStatus::Saved && !Load(file).value.appearance.applicationsLaunchpadOnly;
    ok=ok&&WriteStatusCells(file,{L"combo",L"control",L"clock",L"tray"})==SaveStatus::Saved&&Load(file).value.dock.statusCells.size()==4;
    ok=ok&&!Load(file).value.dock.statusBatteryPercent&&WriteBool(file,"dock","statusBatteryPercent",true)==SaveStatus::Saved&&Load(file).value.dock.statusBatteryPercent;
    if(!ok){std::puts("FAIL menu bar enabled / status cells");return 1;}
    ok = ok && Load(file).value.controlCenter.animatedArtwork && Load(file).value.controlCenter.lyricsDoubleClickSeek;
    ok = ok && WriteBool(file, "controlCenter", "animatedArtwork", false) == SaveStatus::Saved &&
        WriteBool(file, "controlCenter", "lyricsDoubleClickSeek", false) == SaveStatus::Saved &&
        !Load(file).value.controlCenter.animatedArtwork && !Load(file).value.controlCenter.lyricsDoubleClickSeek;
    if (!ok) { std::puts("FAIL music interaction preferences"); return 1; }
    ok = ok && Load(file).value.menuBar.themeGradient;
    ok = ok && WriteBool(file, "menuBar", "themeGradient", false) == SaveStatus::Saved &&
        !Load(file).value.menuBar.themeGradient;
    ok = ok && Load(file).value.menuBar.gradientOpacity == 90 && Load(file).value.menuBar.gradientLength == 100;
    ok = ok && WriteMenuBarInt(file, "gradientOpacity", 55) == SaveStatus::Saved &&
        WriteMenuBarInt(file, "gradientLength", 65) == SaveStatus::Saved &&
        WriteMenuBarInt(file, "gradientLength", 39) == SaveStatus::Invalid &&
        Load(file).value.menuBar.gradientOpacity == 55 && Load(file).value.menuBar.gradientLength == 65;
    if (!ok) { std::puts("FAIL menu bar gradient preference"); return 1; }
    // Output preferences round-trip by endpoint ID in display order, preserving unrelated settings.
    ok = ok && Load(file).value.controlCenter.audioOutputs.empty();
    ok = ok && WriteAudioOutputs(file, {
        {L"endpoint-b", L"\u684c\u9762\u97f3\u7bb1", "stereo", true},
        {L"endpoint-a", L"", "airpods-pro-2-usbc", false}
    }) == SaveStatus::Saved;
    {
        const auto settings = Load(file);
        const auto& outputs = settings.value.controlCenter.audioOutputs;
        ok = ok && settings.status == LoadStatus::Loaded && settings.value.dock.statusBatteryPercent &&
            outputs.size() == 2 && outputs[0].id == L"endpoint-b" &&
            outputs[0].name == L"\u684c\u9762\u97f3\u7bb1" && outputs[0].icon == "stereo" && outputs[0].hidden &&
            outputs[1].id == L"endpoint-a" && outputs[1].name.empty() &&
            outputs[1].icon == "airpods-pro-2-usbc" && !outputs[1].hidden;
    }
    if (!ok) { std::puts("FAIL audio output preferences"); return 1; }
    // Removable Dock items and the right-hand order.
    {const auto d=Load(file).value.dock;ok=ok&&d.showDownloads&&d.showTrash&&d.showDivider&&d.tailOrder.empty();}
    ok=ok&&WriteBool(file,"dock","showTrash",false)==SaveStatus::Saved&&!Load(file).value.dock.showTrash&&Load(file).value.dock.showDownloads;
    ok=ok&&WriteTailOrder(file,{L"downloads",L"status:analog",L"status",L"trash"})==SaveStatus::Saved&&
           Load(file).value.dock.tailOrder==std::vector<std::wstring>{L"downloads",L"status:analog",L"status",L"trash"};
    ok=ok&&WriteTailOrder(file,{L"trash",L"trash"})==SaveStatus::Invalid&&WriteTailOrder(file,{L"status:wifi"})==SaveStatus::Invalid;
    ok=ok&&WriteBool(file,"dock","showTrash",true)==SaveStatus::Saved&&WriteTailOrder(file,{})==SaveStatus::Saved&&Load(file).value.dock.tailOrder.empty();
    if(!ok){std::puts("FAIL removable dock items / tail order");return 1;}
    for(const auto* bad:{"{\"schemaVersion\":1,\"menuBar\":{\"scalePercent\":100.5}}","{\"schemaVersion\":1,\"menuBar\":{\"scalePercent\":\"120\"}}","{\"schemaVersion\":1,\"menuBar\":{\"scalePercent\":0}}"})ok=ok&&put(bad)&&Load(file).status==LoadStatus::Invalid;
    ok=ok&&put("{\"schemaVersion\":1}");
    ok=ok&&Load(file).value.dock.useMacIconMask;
    ok=ok&&put("{\"schemaVersion\":1}")&&!Load(file).value.dock.gameCompatibility;
    ok=ok&&WriteBool(file,"dock","gameCompatibility",true)==SaveStatus::Saved&&Load(file).value.dock.gameCompatibility;
    ok=ok&&WriteBool(file,"dock","gameCompatibility",false)==SaveStatus::Saved&&!Load(file).value.dock.gameCompatibility;
    ok=ok&&put("{\"schemaVersion\":1,\"dock\":{\"gameCompatibility\":1}}")&&Load(file).status==LoadStatus::Invalid;
    // Low latency glass: off by default, persists, a malformed value keeps the default without invalidating the file.
    ok=ok&&put("{\"schemaVersion\":1}")&&!Load(file).value.dock.lowLatencyGlass;
    ok=ok&&WriteBool(file,"dock","lowLatencyGlass",true)==SaveStatus::Saved&&Load(file).value.dock.lowLatencyGlass;
    ok=ok&&WriteBool(file,"dock","lowLatencyGlass",false)==SaveStatus::Saved&&!Load(file).value.dock.lowLatencyGlass;
    ok=ok&&put("{\"schemaVersion\":1,\"dock\":{\"lowLatencyGlass\":\"yes\"}}")&&Load(file).status==LoadStatus::Loaded&&!Load(file).value.dock.lowLatencyGlass;
    if(!ok){std::puts("FAIL game compatibility / low latency glass");return 1;}
    {   // Games list: comments, blank lines, BOM, paths and case; the list file sits beside the settings file.
        const std::wstring list=docky::game::ListPath(file);
        {std::ofstream g(list,std::ios::binary);g<<"\xEF\xBB\xBF# comment\r\n\r\n  TslGame.exe  # PUBG\r\nC:\\Games\\Foo\\Bar.EXE\r\n#VALORANT.exe\r\n";}
        const auto names=docky::game::LoadList(list);
        ok=names==std::vector<std::wstring>{L"tslgame.exe",L"bar.exe"};
        ok=ok&&docky::game::RunningListed(names).empty();  // neither runs here
        wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);std::wstring me=self;me=me.substr(me.find_last_of(L'\\')+1);
        ok=ok&&!docky::game::RunningListed({docky::game::Lower(me)}).empty();  // this test process is found by name
        DeleteFileW(list.c_str());
        ok=ok&&docky::game::EnsureList(list)&&docky::game::LoadList(list).empty();  // template: everything commented out
        DeleteFileW(list.c_str());
        if(!ok){std::puts("FAIL games list");return 1;}
    }
    auto read=[&](){std::ifstream in(file,std::ios::binary);return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());};
    ok=ok&&put("{\"schemaVersion\":1,\"dock\":{\"iconSize\":64}}")&&Load(file).value.dock.iconSize==64;
    ok=ok&&put("{\"schemaVersion\":1}")&&!Load(file).value.controlCenter.mediaArtworkBackground;
    ok=ok&&Load(file).value.controlCenter.musicSensitivity==50;
    for (const int sensitivity : {0, 50, 100})
        ok=ok&&WriteControlCenterInt(file,"musicSensitivity",sensitivity)==SaveStatus::Saved&&
            Load(file).value.controlCenter.musicSensitivity==sensitivity;
    {const auto saved=read();
     ok=ok&&WriteControlCenterInt(file,"musicSensitivity",-1)==SaveStatus::Invalid&&
         WriteControlCenterInt(file,"musicSensitivity",101)==SaveStatus::Invalid&&read()==saved;}
    ok=ok&&WriteBool(file,"controlCenter","mediaArtworkBackground",true)==SaveStatus::Saved&&Load(file).value.controlCenter.mediaArtworkBackground;
    ok=ok&&WriteBool(file,"controlCenter","mediaArtworkBackground",false)==SaveStatus::Saved&&!Load(file).value.controlCenter.mediaArtworkBackground;
    ok=ok&&put("{\"schemaVersion\":1,\"controlCenter\":{\"mediaArtworkBackground\":1}}")&&Load(file).status==LoadStatus::Invalid;
    const char* original="{\"schemaVersion\":1,\"unknown\":{\"large\":9007199254740993,\"raw\":\"\\u0061\"},\"dock\":{\"extra\":[1,2]}}   ";
    ok=ok&&put(original);
    const auto initialRevision=ReadRevision(file);
    ok=ok&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Saved;
    const auto changedRevision=ReadRevision(file);
    ok=ok&&initialRevision!=changedRevision&&WriteBool(file,"dock","diagnostics",true)==SaveStatus::Saved&&ReadRevision(file)==changedRevision;
    auto written=read();ok=ok&&written.find("\"large\":9007199254740993")!=std::string::npos&&written.find("\"raw\":\"\\u0061\"")!=std::string::npos&&Load(file).value.dock.diagnostics;
    ok=ok&&Load(file).value.dock.iconSize==46;
    for(int size : {1, 24, 46, 80, 200, 65535}) ok=ok&&WriteDockInt(file,"iconSizeV2",size)==SaveStatus::Saved&&Load(file).value.dock.iconSize==size;
    {auto saved=read();ok=ok&&WriteDockInt(file,"iconSizeV2",0)==SaveStatus::Invalid&&WriteDockInt(file,"iconSizeV2",65536)==SaveStatus::Invalid&&read()==saved;}
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
