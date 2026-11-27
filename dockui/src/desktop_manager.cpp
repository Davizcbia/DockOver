#include "desktop_settings.h"
#include "liquid_build_id.h"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "taskbar_ownership.h"
#include "taskbar_window.h"
#include "taskbar_guard_sim.h"
#include "taskbar_controller.h"
#include "taskbar_guardian.h"
#include "taskbar_hotkey.h"
#include "update_client.h"
#include "suite_install.h"
#include "json_lite.h"
#include <shellapi.h>
#include <thread>

namespace {
// ---- Automatic update check (Settings > About > Check for updates automatically) ----
// A background thread: 90 s after start, then hourly, it checks GitHub when the last successful check is a day old.
// A new version is installed (Install updates automatically) or announced once with a notification; clicking it
// opens Settings > About. The installer quits the Manager through its quit event, like Menu Bar > Quit.
HANDLE updateStop=nullptr;
constexpr UINT kUpdateBalloonMsg=WM_APP+1;
LRESULT CALLBACK UpdateBalloonProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==kUpdateBalloonMsg){const UINT ev=LOWORD(l);
        if(ev==NIN_BALLOONUSERCLICK){const std::wstring exe=docky::update::ProgramDir()+L"\\LiquidDesktopSettings.exe";ShellExecuteW(nullptr,L"open",exe.c_str(),L"--page about",docky::update::ProgramDir().c_str(),SW_SHOWNORMAL);PostQuitMessage(0);}
        else if(ev==NIN_BALLOONTIMEOUT||ev==NIN_BALLOONHIDE)PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,m,w,l);
}
void NotifyUpdate(const std::string& version){
    WNDCLASSW wc{};wc.lpfnWndProc=UpdateBalloonProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DockyUpdateNotifyV1";RegisterClassW(&wc);
    HWND h=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,wc.hInstance,nullptr);if(!h)return;
    NOTIFYICONDATAW n{sizeof(n)};n.hWnd=h;n.uID=1;n.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_INFO;n.uCallbackMessage=kUpdateBalloonMsg;
    n.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));if(!n.hIcon)n.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(n.szTip,L"DockOver");wcscpy_s(n.szInfoTitle,L"DockOver update available");
    _snwprintf_s(n.szInfo,_TRUNCATE,L"DockOver %hs is available. Click to open Settings and install it.",version.c_str());n.dwInfoFlags=NIIF_INFO;
    if(Shell_NotifyIconW(NIM_ADD,&n)){n.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&n);
        const ULONGLONG until=GetTickCount64()+30000;MSG msg;
        while(GetTickCount64()<until){const DWORD r=MsgWaitForMultipleObjects(1,&updateStop,FALSE,500,QS_ALLINPUT);if(r==WAIT_OBJECT_0)break;
            bool quit=false;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}DispatchMessageW(&msg);}if(quit)break;}
        Shell_NotifyIconW(NIM_DELETE,&n);}
    DestroyWindow(h);
}
void UpdateLoop(std::wstring settingsFile){
    namespace up=docky::update;
    DWORD wait=90000;
    while(WaitForSingleObject(updateStop,wait)==WAIT_TIMEOUT){
        wait=3600000;
        const auto pref=liquid::settings::Load(settingsFile).value.updates;
        if(!pref.autoCheck)continue;
        auto st=up::LoadState();
        if(up::UnixNow()-st.lastCheck<23*3600)continue;
        const auto r=up::Check();
        if(r.status==up::CheckStatus::Failed){std::printf("update: check failed: %ls\n",r.error.c_str());std::fflush(stdout);continue;}
        st=up::LoadState();st.lastCheck=up::UnixNow();st.latest=r.status==up::CheckStatus::Available?r.release.version:std::string();up::SaveState(st);
        std::printf("update: checked; status=%d latest=%s current=%s\n",static_cast<int>(r.status),r.release.version.c_str(),up::CurrentVersion());std::fflush(stdout);
        if(r.status!=up::CheckStatus::Available)continue;
        if(pref.autoInstall){
            const std::wstring zip=up::WorkDir()+L"\\"+dock::json::FromUtf8(r.release.assetName);std::wstring err;
            std::atomic<bool> cancel{false};
            if(up::Download(r.release,zip,nullptr,&cancel,err)&&up::StartInstall(zip,r.release.version,false,false,err)){
                std::printf("update: installing %s; docky will restart\n",r.release.version.c_str());std::fflush(stdout);return;}
            std::printf("update: automatic install failed: %ls\n",err.c_str());std::fflush(stdout);
        }
        if(st.notified!=r.release.version){st.notified=r.release.version;up::SaveState(st);NotifyUpdate(r.release.version);}
    }
}
HANDLE stopEvent=nullptr;
HANDLE doneEvent=nullptr;  // set after the taskbar was restored on exit
BOOL WINAPI OnConsole(DWORD type){
    if(type==CTRL_C_EVENT || type==CTRL_BREAK_EVENT || type==CTRL_CLOSE_EVENT || type==CTRL_LOGOFF_EVENT || type==CTRL_SHUTDOWN_EVENT){
        if(stopEvent)SetEvent(stopEvent);
        // Close/logoff/shutdown terminate the process when this returns: give the main loop time to restore the
        // taskbar and stop the guardian first (the guardian still recovers if this is cut short).
        if(type>=CTRL_CLOSE_EVENT && doneEvent)WaitForSingleObject(doneEvent,4500);
        return TRUE;}
    return FALSE;
}
std::wstring LocalPath(const wchar_t* tail) {
    PWSTR p=nullptr; if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&p)))return {};
    std::wstring result(p);CoTaskMemFree(p);result+=tail;return result;
}
std::wstring AdjacentExe(const wchar_t* name){
    wchar_t p[32768]{};DWORD n=GetModuleFileNameW(nullptr,p,32768);if(!n||n>=32768)return {};
    std::wstring path(p,n);const auto slash=path.find_last_of(L"\\/");return slash==std::wstring::npos?L"":path.substr(0,slash+1)+name;
}
// Settings > General > "Start DockOver Desktop with DockOver": one-way binding. The Manager starts DockOver Desktop
// (widgets and Stacks) once the Dock and Menu Bar are up; DockOver Desktop never starts the Dock. Release folders keep
// "DockOver Desktop.exe" beside runtime\ (the Manager's folder); build folders keep DockOverDesktop.exe next to it.
// Started detached and not owned: quitting the Dock leaves DockOver Desktop running (it is quit on its own).
void LaunchDesktopApp(){
    if(HANDLE running=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\DockyDesktopV1SingleInstance")){CloseHandle(running);std::printf("manager: DockOver Desktop already running\n");return;}
    std::wstring exe=AdjacentExe(L"..\\DockOver Desktop.exe");
    if(exe.empty()||GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES)exe=AdjacentExe(L"DockOverDesktop.exe");
    if(exe.empty()||GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES){std::printf("manager: DockOver Desktop not found; not started\n");return;}
    std::wstring full(32768,L'\0');const DWORD n=GetFullPathNameW(exe.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
    if(n&&n<full.size())exe=full.substr(0,n);
    const auto dir=exe.substr(0,exe.find_last_of(L"\\/"));
    std::wstring cmd=L"\""+exe+L"\"";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    // Break away from a job the Manager may run in, so DockOver Desktop outlives it; fall back when the job forbids it.
    if(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_BREAKAWAY_FROM_JOB,nullptr,dir.c_str(),&si,&pi)||
       CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,dir.c_str(),&si,&pi)){
        std::printf("manager: started DockOver Desktop pid=%lu\n",pi.dwProcessId);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    else std::printf("manager: DockOver Desktop launch failed error=%lu\n",GetLastError());
}
// Settings > Control Center > "Music wallpaper": docky-now-playing.exe --wallpaper shows the playing Apple Music artwork
// behind the desktop icons. The Manager starts it while the setting is on (checked every second, so it also comes back
// after an Explorer restart), and asks it to close when the setting goes off or DockOver quits.
// The Manager has no console when started from Explorer: wallpaper decisions also go to the Now Playing log.
void WallpaperLog(const char* text,unsigned long value=0){
    wchar_t base[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return;
    const std::wstring path=std::wstring(base)+L"\\LiquidDesktop\\artwork\\now-playing.log";
    HANDLE f=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,0,nullptr);if(f==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME st;GetLocalTime(&st);char line[256];const int n=sprintf_s(line,"%02d:%02d:%02d manager: %s %lu\r\n",st.wHour,st.wMinute,st.wSecond,text,value);
    DWORD written=0;if(n>0)WriteFile(f,line,static_cast<DWORD>(n),&written,nullptr);CloseHandle(f);
    std::printf("manager: music wallpaper: %s %lu\n",text,value);std::fflush(stdout);
}
void SyncMusicWallpaper(bool on){
    const auto exe=AdjacentExe(L"docky-now-playing.exe");
    if (!on) {
        // The standalone Music Player uses the same control class and wallpaper mutex.
        for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"DockyNowPlayingWallpaperCtl", nullptr));) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) continue;
            wchar_t image[32768]{};
            DWORD length = ARRAYSIZE(image);
            const bool ours = QueryFullProcessImageNameW(process, 0, image, &length) && _wcsicmp(image, exe.c_str()) == 0;
            CloseHandle(process);
            if (ours) PostMessageW(w, WM_CLOSE, 0, 0);
        }
        return;
    }
    if(HANDLE running=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\DockyNowPlayingWallpaper")){CloseHandle(running);return;}
    if(exe.empty()||GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES){WallpaperLog("docky-now-playing.exe not found");return;}
    std::wstring cmd=L"\""+exe+L"\" --wallpaper";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    const auto dir=exe.substr(0,exe.find_last_of(L"\\/"));
    if(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,dir.c_str(),&si,&pi)){
        WallpaperLog("started wallpaper pid",pi.dwProcessId);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    else WallpaperLog("wallpaper launch failed, error",GetLastError());
}
// Settings > Control Center shortcuts (global, registered on the Manager thread like the taskbar shortcut).
constexpr int kShortcutBase=0x4460;  // +0 desktop lyrics, +1 music wallpaper, +2 hide DockOver Desktop, +3 full-screen Now Playing
void ApplyShortcuts(const liquid::settings::ControlCenter& cc,int (&active)[4]){
    const int wanted[4]={cc.shortcutLyrics,cc.shortcutArtwork,cc.shortcutDesktop,cc.shortcutNowPlaying};
    for(int i=0;i<4;++i){
        if(wanted[i]==active[i])continue;
        if(active[i])UnregisterHotKey(nullptr,kShortcutBase+i);
        active[i]=0;
        if(wanted[i]&&RegisterHotKey(nullptr,kShortcutBase+i,UINT(wanted[i]>>8)|MOD_NOREPEAT,UINT(wanted[i]&0xFF)))active[i]=wanted[i];
        else if(wanted[i]){WallpaperLog("shortcut could not be registered (used by another program?)",static_cast<unsigned long>(wanted[i]));active[i]=-wanted[i];}
    }
}
// DockOver Desktop's host window (class DockyDesktopHostV1); it shows or hides its widgets and Stacks on this message.
void ToggleDesktopApp(){
    static const UINT toggle=RegisterWindowMessageW(L"DockyDesktopToggleVisible");
    EnumWindows([](HWND w,LPARAM)->BOOL{wchar_t cls[32]{};GetClassNameW(w,cls,32);if(!wcscmp(cls,L"DockyDesktopHostV1"))PostMessageW(w,toggle,0,0);return TRUE;},0);
}
void RunShortcut(int index,const std::wstring& settingsFile){
    const auto cc=liquid::settings::Load(settingsFile).value.controlCenter;
    if(index==0)liquid::settings::WriteBool(settingsFile,"controlCenter","wallpaperLyrics",!cc.wallpaperLyrics);
    else if(index==1)liquid::settings::WriteBool(settingsFile,"controlCenter","musicWallpaper",!cc.musicWallpaper);
    else if(index==2)ToggleDesktopApp();
    else if(index==3){const auto exe=AdjacentExe(L"docky-now-playing.exe");if(!exe.empty())ShellExecuteW(nullptr,L"open",exe.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
    WallpaperLog("shortcut",static_cast<unsigned long>(index));
}
bool MatchingBuild(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return data.find(LIQUID_BUILD_MARKER) != std::string::npos;
}
bool IsAdjacentDock(const std::wstring& path) {
    wchar_t a[32768]{}, b[32768]{};
    const auto expected=AdjacentExe(L"LiquidDock.exe");
    DWORD na=GetFullPathNameW(path.c_str(),32768,a,nullptr);
    DWORD nb=GetFullPathNameW(expected.c_str(),32768,b,nullptr);
    return na && nb && na<32768 && nb<32768 && _wcsicmp(a,b)==0;
}
bool InstanceExists(const wchar_t* name) {
    HANDLE h=OpenMutexW(SYNCHRONIZE,FALSE,name);
    if(h){CloseHandle(h);return true;}
    return GetLastError()!=ERROR_FILE_NOT_FOUND;
}
void StartupError(const wchar_t* message) {
    std::fwprintf(stderr,L"manager: %ls\n",message);
    MessageBoxW(nullptr,message,L"DockOver",MB_OK|MB_ICONWARNING);
}
// A component is launched with a dedicated process handle; Menu Bar can later supply its own spec.
// Graceful close of a component the Manager launched: WM_CLOSE to that PID's main window class only.
struct CloseTarget { DWORD pid; const wchar_t* cls; int posted; };
BOOL CALLBACK PostCloseToOwned(HWND h, LPARAM l){
    auto* t=reinterpret_cast<CloseTarget*>(l);DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[64]{};
    if(pid==t->pid && GetClassNameW(h,cls,64) && wcscmp(cls,t->cls)==0){PostMessageW(h,WM_CLOSE,0,0);++t->posted;}
    return TRUE;
}
int CloseOwned(HANDLE process,const wchar_t* cls){if(!process)return 0;CloseTarget t{GetProcessId(process),cls,0};EnumWindows(PostCloseToOwned,reinterpret_cast<LPARAM>(&t));return t.posted;}
struct FindTarget { DWORD pid; const wchar_t* cls; HWND window; };
BOOL CALLBACK FindOwnedWindow(HWND h,LPARAM l){auto* t=reinterpret_cast<FindTarget*>(l);DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[64]{};
    if(pid==t->pid && GetClassNameW(h,cls,64) && wcscmp(cls,t->cls)==0){t->window=h;return FALSE;}return TRUE;}
HWND FindOwned(HANDLE process,const wchar_t* cls){if(!process)return nullptr;FindTarget t{GetProcessId(process),cls,nullptr};EnumWindows(FindOwnedWindow,reinterpret_cast<LPARAM>(&t));return t.window;}

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring result=L"\""; size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        if(c==L'"'){result.append(slashes*2+1,L'\\');result+=c;}
        else {result.append(slashes,L'\\');result+=c;}
        slashes=0;
    }
    result.append(slashes*2,L'\\');return result+L"\"";
}
struct ComponentSpec { const wchar_t* name; std::wstring executable; std::wstring arguments; };
bool LaunchComponent(const ComponentSpec& component, PROCESS_INFORMATION& process, HANDLE inherited=nullptr) {
    auto command=component.arguments;
    if(!inherited){STARTUPINFOW si{};si.cb=sizeof(si);return CreateProcessW(component.executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&process)!=FALSE;}
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);if(!bytes)return false;
    std::vector<unsigned char> storage(bytes);auto attrs=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attrs,1,0,&bytes))return false;
    BOOL ok=UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&inherited,sizeof(inherited),nullptr,nullptr);
    if(ok){STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=attrs;
        ok=CreateProcessW(component.executable.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&process);}
    const DWORD error=GetLastError();DeleteProcThreadAttributeList(attrs);SetLastError(error);return ok!=FALSE;
}
void InspectRecord(const std::wstring& path){
    if(path.empty()){std::puts("taskbar record: path unavailable; no taskbar action");return;}
    DWORD attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES){DWORD e=GetLastError();std::printf("taskbar record: %s (%lu); no taskbar action\n",e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND?"not present":"unavailable",e);return;}
    liquid::desktop::TaskbarOperationStore store;std::wstring error;
    if(!store.OpenForInspection(path,error)){std::fwprintf(stderr,L"taskbar record: cannot inspect safely: %ls; no taskbar action\n",error.c_str());return;}
    const auto& op=store.Current();
    if(op.phase==liquid::desktop::OperationPhase::Conflict)
        std::printf("taskbar record: CONFLICT original=%d expected=%d last_observed=%d; no automatic restore or abandon\n",op.original,op.target,op.lastObserved);
    else std::printf("taskbar record: phase=%d; no taskbar action\n",static_cast<int>(op.phase));
}
}
// Production recovery guardian: launched with only its three inherited handles; hiding requires its ready signal.
struct Guardian {
    HANDLE process=nullptr,stop=nullptr,ready=nullptr;bool readyOk=false;std::wstring error;
    bool Start(const std::wstring& record){
        if(process)return readyOk;
        const auto exe=AdjacentExe(L"docky-taskbar-guardian.exe");
        if(exe.empty()||!MatchingBuild(exe)){error=L"guardian executable missing or from another build";return false;}
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        stop=CreateEventW(&sa,TRUE,FALSE,nullptr);ready=CreateEventW(&sa,TRUE,FALSE,nullptr);HANDLE self=nullptr;
        if(!stop||!ready||!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&self,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)){error=L"guardian handles unavailable";Close();if(self)CloseHandle(self);return false;}
        HANDLE list[3]{self,stop,ready};SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
        auto attrs=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());bool ok=bytes&&InitializeProcThreadAttributeList(attrs,1,0,&bytes);
        if(ok){ok=UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,list,sizeof(list),nullptr,nullptr)!=FALSE;
            if(ok){std::wstring cmd=QuoteArgument(exe)+L" --manager "+std::to_wstring(reinterpret_cast<ULONG_PTR>(self))+L" --stop "+std::to_wstring(reinterpret_cast<ULONG_PTR>(stop))+
                    L" --ready "+std::to_wstring(reinterpret_cast<ULONG_PTR>(ready))+L" --record "+QuoteArgument(record);
                STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=attrs;PROCESS_INFORMATION pi{};
                ok=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=FALSE;
                if(ok){process=pi.hProcess;CloseHandle(pi.hThread);}}
            DeleteProcThreadAttributeList(attrs);}
        CloseHandle(self);
        if(!ok){error=L"guardian could not be started ("+std::to_wstring(GetLastError())+L")";Close();return false;}
        HANDLE waits[]{ready,process};const DWORD w=WaitForMultipleObjects(2,waits,FALSE,5000);   // readiness handshake
        readyOk=w==WAIT_OBJECT_0&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;
        if(!readyOk){error=w==WAIT_TIMEOUT?L"guardian handshake timed out":L"guardian exited during handshake";Stop();return false;}
        error.clear();return true;
    }
    bool Ready() const {return readyOk&&process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
    void Stop(){if(process){SetEvent(stop);if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0)std::puts("guardian: did not exit after stop (left running; it exits with the Manager)");}Close();}
    void Close(){for(HANDLE* h:{&process,&stop,&ready}){if(*h)CloseHandle(*h);*h=nullptr;}readyOk=false;}
};
// Global toggle shortcut, registered on the Manager thread (WM_HOTKEY arrives in its message queue).
constexpr int kHotkeyId=0x4454;
void ApplyHotkey(const liquid::settings::Taskbar& t,docky::taskbar::Status& status,bool& registered){
    if(registered){UnregisterHotKey(nullptr,kHotkeyId);registered=false;}
    const UINT mods=static_cast<UINT>(t.hotkeyModifiers),vk=static_cast<UINT>(t.hotkeyKey);
    status.hotkeyText=docky::hotkey::Text(mods,vk);
    if(!t.hotkeyEnabled||!vk){status.hotkey=L"disabled";return;}
    if(docky::hotkey::Validate(mods,vk)!=docky::hotkey::Validity::Ok){status.hotkey=L"invalid";status.message=std::wstring(L"Shortcut not used: ")+docky::hotkey::ValidityText(docky::hotkey::Validate(mods,vk));return;}
    if(RegisterHotKey(nullptr,kHotkeyId,mods|MOD_NOREPEAT,vk)){registered=true;status.hotkey=L"registered";std::printf("hotkey: registered %ls\n",status.hotkeyText.c_str());}
    else {const DWORD e=GetLastError();status.hotkey=L"conflict";status.message=status.hotkeyText+L" is already used by another app. Choose another shortcut in Settings.";
        std::printf("hotkey: %ls not registered (error %lu)\n",status.hotkeyText.c_str(),e);}
}
// --hide-taskbar (V0.3): the Manager is the single owner of taskbar state. Pre-flight refuses when the
// legacy autohide record is unfinished/Conflict (kept as is) or the shell state is unreadable; hiding
// itself is window-level (taskbar_window: write-ahead record, SW_HIDE once, no ABM_SETSTATE/registry).
bool TaskbarPreflight(const std::wstring& recordPath){
    const auto snap=liquid::desktop::CaptureTaskbarSnapshot();
    if(!snap.valid||!snap.primaryWindowFound){std::puts("taskbar: hide refused: current state unreadable; no taskbar action");return false;}
    if(!recordPath.empty() && GetFileAttributesW(recordPath.c_str())!=INVALID_FILE_ATTRIBUTES){
        liquid::desktop::TaskbarOperationStore store;std::wstring error;
        if(!store.OpenForInspection(recordPath,error)){std::puts("taskbar: hide refused: record cannot be inspected; no taskbar action");return false;}
        const auto phase=store.Current().phase;
        if(phase==liquid::desktop::OperationPhase::Conflict){std::puts("taskbar: hide refused: Conflict record present (kept as is); no taskbar action");return false;}
        if(phase!=liquid::desktop::OperationPhase::Idle&&phase!=liquid::desktop::OperationPhase::Restored&&phase!=liquid::desktop::OperationPhase::Abandoned){
            std::printf("taskbar: hide refused: unfinished record phase=%d; no taskbar action\n",static_cast<int>(phase));return false;}
    }
    return true;
}
bool RestoreWindows(const std::wstring& path,const char* when){
    using namespace liquid::desktop;
    DWORD tmp=GetFileAttributesW((path+L".tmp").c_str());
    if(tmp!=INVALID_FILE_ATTRIBUTES || (GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND)){
        std::puts("taskbar: restore stopped; temporary record exists or is inaccessible");return false;
    }
    TaskbarWindowOperation op;std::wstring error;
    if(!LoadWindowOperation(path,op,error) || !IsConservativeRecoveryRecord(op)){
        std::puts("taskbar: restore stopped; invalid or ambiguous record preserved");return false;
    }
    if(op.phase==RecordPhase::Closed)return true;
    DWORD shellPid=0;if(HWND shell=GetShellWindow())GetWindowThreadProcessId(shell,&shellPid);
    if(!shellPid){std::puts("taskbar: restore stopped; shell identity unavailable");return false;}
    auto ops=WindowOps::Real();const auto observe=ops.observe;
    ops.observe=[observe,shellPid](const TaskbarWindowRecord& wanted,TaskbarWindowRecord& observed){return wanted.pid==shellPid && observe(wanted,observed);};
    std::wstring log;const bool ok=RestoreTaskbarWindows(path,ops,log);
    std::printf("taskbar: restore (%s) %s\n",when,ok?"ok":"stopped");if(!log.empty())std::printf("%ls",log.c_str());std::fflush(stdout);return ok;
}
int wmain(int argc,wchar_t** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // physical taskbar and work-area rects
    std::wstring settingsFile=liquid::settings::DefaultPath();std::wstring dockPath, recordPath, dockConfig, dockLog;unsigned managerSeconds=0,dockSeconds=0;bool hideTaskbar=false, dockDiag=false;std::wstring windowRecord, guardSimDir;bool guardSimCrash=false;
    for(int i=1;i<argc;++i){std::wstring a=argv[i];
        if(a==L"--dock-path" && i+1<argc)dockPath=argv[++i];
        else if(a==L"--dock-config" && i+1<argc)dockConfig=argv[++i];
        else if(a==L"--dock-log" && i+1<argc)dockLog=argv[++i];
        else if(a==L"--dock-diag")dockDiag=true;
        else if(a==L"--hide-taskbar")hideTaskbar=true;
        else if(a==L"--guard-sim-dir" && i+1<argc)guardSimDir=argv[++i];
        else if(a==L"--guard-sim-crash-after-hide")guardSimCrash=true;
        else if(a==L"--taskbar-window-record" && i+1<argc)windowRecord=argv[++i];
        else if(a==L"--record" && i+1<argc)recordPath=argv[++i];
        else if(a==L"--settings-path" && i+1<argc)settingsFile=argv[++i];  // isolated shared settings (tests)
        else if((a==L"--exit-after" || a==L"--dock-exit-after") && i+1<argc){wchar_t* end=nullptr;unsigned long n=std::wcstoul(argv[++i],&end,10);if(!end||*end||n<1||n>3600)return 2;if(a==L"--exit-after")managerSeconds=n;else dockSeconds=n;}
        else {std::fputs("usage: LiquidDesktopManager [--dock-path exe] [--dock-config path] [--dock-log path] [--dock-diag] [--record path] [--exit-after seconds] [--dock-exit-after seconds] [--hide-taskbar] [--taskbar-window-record path] [--settings-path path]\n",stderr);return 2;}}
    if(guardSimCrash && guardSimDir.empty()){std::fputs("guard-sim: crash injection requires explicit simulation directory\n",stderr);return 2;}
    HANDLE single=CreateMutexW(nullptr,FALSE,L"Local\\LiquidDesktopManagerV05SingleInstance");
    if(!single){std::printf("manager: mutex failed error=%lu\n",GetLastError());return 1;}
    if(GetLastError()==ERROR_ALREADY_EXISTS){std::puts("manager: already running");CloseHandle(single);return 3;}
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent){CloseHandle(single);return 1;}
    doneEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE quitEvent=CreateEventW(nullptr,TRUE,FALSE,L"Local\\LiquidDesktopManagerQuitV1");  // set by Menu Bar "Quit Liquid Desktop"
    SetConsoleCtrlHandler(OnConsole,TRUE);
    HANDLE settingsEvent=CreateEventW(nullptr,FALSE,FALSE,L"Local\\LiquidDesktopSettingsOpenV1");
    HANDLE settingsChild=nullptr;
    const auto sharedSettings=liquid::settings::Load(settingsFile);
    std::printf("manager: shared settings load status=%d\n",static_cast<int>(sharedSettings.status));
    if(recordPath.empty())recordPath=LocalPath(L"\\LiquidDesktop\\taskbar-operation-v1.txt");
    InspectRecord(recordPath);
    if(windowRecord.empty())windowRecord=liquid::desktop::DefaultTaskbarWindowRecordPath();
    // Always finish an unfinished window record first (Manager crash recovery); Conflict/Pending are kept.
    if(!windowRecord.empty()){std::wstring log;const auto r=docky::taskbar::RecoverTaskbarRecord(windowRecord,log);
        std::printf("taskbar: startup recovery result=%d\n",static_cast<int>(r));if(!log.empty())std::printf("%ls",log.c_str());}

    if(dockPath.empty())dockPath=AdjacentExe(L"LiquidDock.exe");
    const auto expectedMenu=AdjacentExe(L"LiquidMenuBar.exe");
    if(!IsAdjacentDock(dockPath) || !MatchingBuild(dockPath) || !MatchingBuild(expectedMenu)){
        StartupError(L"Dock and Menu Bar must come from the same build as Desktop Manager. Rebuild all components together.");
        if(quitEvent)CloseHandle(quitEvent);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 4;
    }
    if(InstanceExists(L"Local\\LiquidDockV01SingleInstance") ||
       InstanceExists(L"Local\\LiquidDesktopMenuBarV01SingleInstance")){
        StartupError(L"An independent Dock or Menu Bar is already running, or its status cannot be checked. Close it yourself and start DockOver again. No process was closed.");
        if(quitEvent)CloseHandle(quitEvent);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 3;
    }
    HANDLE dockInstance=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\LiquidDockV01SingleInstance");
    DWORD openError=dockInstance?ERROR_SUCCESS:GetLastError(); HANDLE child=nullptr, firstFrameEvent=nullptr;
    if(dockInstance || openError==ERROR_ACCESS_DENIED){
        std::puts("manager: Dock already running or mutex access denied; leaving external process untouched");if(dockInstance)CloseHandle(dockInstance);
    }else if(openError!=ERROR_FILE_NOT_FOUND){std::printf("manager: Dock mutex check failed error=%lu; not starting duplicate\n",openError);
    }else {
        if(dockPath.empty())dockPath=AdjacentExe(L"LiquidDock.exe");
        if(dockPath.empty() || GetFileAttributesW(dockPath.c_str())==INVALID_FILE_ATTRIBUTES){std::fputs("manager: LiquidDock.exe not found\n",stderr);SetConsoleCtrlHandler(OnConsole,FALSE);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 4;}
        std::wstring args=L"\""+dockPath+L"\" --material liquid --backdrop live";
        if(dockSeconds)args+=L" --exit-after "+std::to_wstring(dockSeconds);
        if(!dockConfig.empty())args+=L" --config "+QuoteArgument(dockConfig);
        const auto log=dockLog.empty()?LocalPath(L"\\LiquidDock\\manager-preview.log"):dockLog;
        if(!log.empty())args+=L" --log "+QuoteArgument(log);
        if(dockDiag)args+=L" --diag";
        SECURITY_ATTRIBUTES inherit{};inherit.nLength=sizeof(inherit);inherit.bInheritHandle=TRUE;
        firstFrameEvent=CreateEventW(&inherit,TRUE,FALSE,nullptr);const std::wstring unguardedArgs=args;
        if(firstFrameEvent)args+=L" --manager-ready-handle "+std::to_wstring(reinterpret_cast<ULONG_PTR>(firstFrameEvent));
        else std::puts("manager: first-frame event unavailable; taskbar hiding remains disabled");
        ComponentSpec dockComponent{L"Dock",dockPath,args}; PROCESS_INFORMATION pi{};
        bool launched=LaunchComponent(dockComponent,pi,firstFrameEvent);
        if(!launched && firstFrameEvent){
            // Preserve ordinary Dock startup if restricted handle inheritance is unavailable.
            std::printf("manager: first-frame channel failed error=%lu; retrying without channel\n",GetLastError());
            CloseHandle(firstFrameEvent);firstFrameEvent=nullptr;dockComponent.arguments=unguardedArgs;
            launched=LaunchComponent(dockComponent,pi);
        }
        if(!launched){
            std::printf("manager: Dock launch failed error=%lu\n",GetLastError());if(firstFrameEvent)CloseHandle(firstFrameEvent);SetConsoleCtrlHandler(OnConsole,FALSE);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 4;}
        if(firstFrameEvent)SetHandleInformation(firstFrameEvent,HANDLE_FLAG_INHERIT,0);
        child=pi.hProcess;CloseHandle(pi.hThread);std::printf("manager: launched Dock pid=%lu\n",pi.dwProcessId);
    }
    if(!child){
        StartupError(L"Dock could not be started because another instance appeared or its status is unavailable. No independent process was closed.");
        if(quitEvent)CloseHandle(quitEvent);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 4;
    }
    // The Menu Bar is optional (Settings > Menu Bar > Show menu bar): when it is off its process does not run at all and
    // the Dock's status group hosts the status items. It is started here and whenever the setting turns it back on.
    HANDLE menuChild=nullptr;
    auto startMenuBar=[&]()->bool{
        HANDLE menuInstance=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\LiquidDesktopMenuBarV01SingleInstance");
        const DWORD menuError=menuInstance?ERROR_SUCCESS:GetLastError();
        if(menuInstance||menuError==ERROR_ACCESS_DENIED){std::puts("manager: an independent Menu Bar is running; leaving it untouched");if(menuInstance)CloseHandle(menuInstance);return false;}
        if(menuError!=ERROR_FILE_NOT_FOUND){std::printf("manager: Menu Bar mutex check failed error=%lu\n",menuError);return false;}
        const auto menuPath=AdjacentExe(L"LiquidMenuBar.exe");PROCESS_INFORMATION pi{};
        const auto menuLog=LocalPath(L"\\LiquidDock\\menubar.log");  // window-drag / full-screen decisions are only visible here
        ComponentSpec spec{L"Menu Bar",menuPath,L"\""+menuPath+L"\""+(menuLog.empty()?std::wstring():L" --log "+QuoteArgument(menuLog))};
        if(!menuPath.empty() && LaunchComponent(spec,pi)){menuChild=pi.hProcess;CloseHandle(pi.hThread);std::printf("manager: launched Menu Bar pid=%lu\n",pi.dwProcessId);std::fflush(stdout);return true;}
        std::printf("manager: Menu Bar launch failed error=%lu; Dock continues\n",GetLastError());std::fflush(stdout);return false;
    };
    auto stopMenuBar=[&]{
        if(!menuChild)return;
        const int posted=CloseOwned(menuChild,L"LiquidDesktopMenuBarV01");
        const DWORD r=WaitForSingleObject(menuChild,8000);
        std::printf("manager: Menu Bar turned off (WM_CLOSE posted=%d, %s)\n",posted,r==WAIT_OBJECT_0?"exited":"still running after 8 s (left untouched)");std::fflush(stdout);
        CloseHandle(menuChild);menuChild=nullptr;
    };
    bool menuBarEnabled=liquid::settings::Load(settingsFile).value.menuBar.enabled;
    if(menuBarEnabled)startMenuBar();
    else std::puts("manager: Menu Bar is turned off in Settings; not started");
    if(!managerSeconds&&settingsFile==liquid::settings::DefaultPath()&&liquid::settings::Load(settingsFile).value.dock.launchDesktop)LaunchDesktopApp();
    const bool normalSession=!managerSeconds&&settingsFile==liquid::settings::DefaultPath();
    bool musicWallpaper=normalSession&&liquid::settings::Load(settingsFile).value.controlCenter.musicWallpaper;
    auto wallpaperRevision=liquid::settings::ReadRevision(settingsFile);ULONGLONG wallpaperCheck=0;
    int shortcutsActive[4]{};
    if (normalSession) {
        const auto dir = docky::update::ProgramDir();
        const auto separator = dir.find_last_of(L"\\/");
        if (separator != std::wstring::npos && _wcsicmp(dir.c_str() + separator + 1, L"runtime") == 0)
            docky::suite::Register(dir.substr(0, separator));
        docky::suite::StopStandaloneWindows();
    }
    if(normalSession)ApplyShortcuts(liquid::settings::Load(settingsFile).value.controlCenter,shortcutsActive);
    WallpaperLog(normalSession?(musicWallpaper?"session start, setting on":"session start, setting off"):"not a normal session");
    std::fflush(stdout);
    // Production taskbar integration: nothing is hidden until the recovery guardian has completed its ready
    // handshake (started after the Dock's first frame). Preference (hideOnStartup) and current state are separate.
    namespace tb=docky::taskbar;
    Guardian guardian;tb::Status status;status.managerPid=GetCurrentProcessId();const auto statusPath=tb::DefaultStatusPath();
    tb::Controller taskbar(windowRecord,tb::Backend::Real([&guardian]{return guardian.Ready();}));taskbar.Adopt();
    taskbar.SetFreeWorkArea(liquid::settings::Load(settingsFile).value.taskbar.freeWorkArea);  // opt-in; off by default
    bool hotkeyRegistered=false;MSG peek{};PeekMessageW(&peek,nullptr,0,0,PM_NOREMOVE);  // create this thread's queue for WM_HOTKEY
    auto taskbarSettings=liquid::settings::Load(settingsFile).value.taskbar;
    auto settingsRevision=liquid::settings::ReadRevision(settingsFile);
    const bool hideAtStart=hideTaskbar||taskbarSettings.hideOnStartup;
    auto publish=[&](const std::wstring& message){status.state=tb::StateName(taskbar.Current());
        status.guardian=guardian.Ready()?L"ready":guardian.process?L"starting":guardian.error.empty()?L"stopped":L"failed";
        if(!message.empty())status.message=message;tb::WriteStatus(statusPath,status);
        std::printf("taskbar: state=%ls guardian=%ls hotkey=%ls %ls\n",status.state.c_str(),status.guardian.c_str(),status.hotkey.c_str(),message.c_str());std::fflush(stdout);};
    auto act=[&](int op,const wchar_t* source){std::wstring why;const bool ok=op==0?taskbar.Hide(why):op==1?taskbar.Show(why):taskbar.Toggle(why);
        const std::wstring done=std::wstring(source)+L": "+(op==0?L"hide":op==1?L"show":L"toggle")+(ok?L" ok":L" failed");
        publish(ok?done:done+L" - "+why);};
    auto startGuardian=[&]{if(windowRecord.empty()){guardian.error=L"record path unavailable";publish(L"Taskbar control unavailable: record path missing.");return;}
        const bool ok=guardian.Start(windowRecord);
        publish(ok?L"Recovery guardian ready.":L"Taskbar hiding disabled: "+guardian.error+L".");
        if(ok&&hideAtStart)act(0,L"startup");};
    ApplyHotkey(taskbarSettings,status,hotkeyRegistered);
    tb::HotkeySuspension suspension;  // shortcut recording in Settings pauses the global hotkey
    auto reloadHotkey=[&]{const auto current=liquid::settings::Load(settingsFile);
        if(current.status==liquid::settings::LoadStatus::Loaded||current.status==liquid::settings::LoadStatus::Missing)taskbarSettings=current.value.taskbar;
        status.message.clear();ApplyHotkey(taskbarSettings,status,hotkeyRegistered);settingsRevision=liquid::settings::ReadRevision(settingsFile);};
    HANDLE hideEvent=CreateEventW(nullptr,FALSE,FALSE,tb::kHideEvent),showEvent=CreateEventW(nullptr,FALSE,FALSE,tb::kShowEvent),toggleEvent=CreateEventW(nullptr,FALSE,FALSE,tb::kToggleEvent);
    publish(L"");
    if(!firstFrameEvent)startGuardian();  // no first-frame channel: start now (hiding still waits for readiness)
    docky::guard_sim::Session guardSim;
    const ULONGLONG started=GetTickCount64();
    // Automatic updates only for a normal session (not tests with --exit-after or an isolated settings file).
    std::thread updateThread;
    if(!managerSeconds&&settingsFile==liquid::settings::DefaultPath()){updateStop=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(updateStop)updateThread=std::thread(UpdateLoop,settingsFile);}
    for(;;){HANDLE handles[16]={stopEvent};DWORD count=1;DWORD quitIndex=MAXDWORD;if(quitEvent){quitIndex=count;handles[count++]=quitEvent;}DWORD settingsIndex=MAXDWORD;if(settingsEvent){settingsIndex=count;handles[count++]=settingsEvent;}DWORD dockIndex=0,menuIndex=0;if(child){dockIndex=count;handles[count++]=child;}if(menuChild){menuIndex=count;handles[count++]=menuChild;}DWORD readyIndex=MAXDWORD;if(firstFrameEvent){readyIndex=count;handles[count++]=firstFrameEvent;}DWORD guardIndex=MAXDWORD;if(guardSim.Process()){guardIndex=count;handles[count++]=guardSim.Process();}
        DWORD guardianIndex=MAXDWORD;if(guardian.process){guardianIndex=count;handles[count++]=guardian.process;}
        DWORD cmdIndex[3]{MAXDWORD,MAXDWORD,MAXDWORD};HANDLE cmds[3]{hideEvent,showEvent,toggleEvent};for(int c=0;c<3;++c)if(cmds[c]){cmdIndex[c]=count;handles[count++]=cmds[c];}
        DWORD leaseChangedIndex=MAXDWORD;if(suspension.ChangedEvent()){leaseChangedIndex=count;handles[count++]=suspension.ChangedEvent();}
        DWORD leaseIndex=MAXDWORD;if(suspension.LeaseHandle()){leaseIndex=count;handles[count++]=suspension.LeaseHandle();}
        DWORD timeout=1000;
        if(managerSeconds){ULONGLONG elapsed=GetTickCount64()-started;ULONGLONG limit=1000ull*managerSeconds;if(elapsed>=limit)break;timeout=static_cast<DWORD>(std::min<ULONGLONG>(1000,limit-elapsed));}
        DWORD wait=MsgWaitForMultipleObjects(count,handles,FALSE,timeout,QS_ALLINPUT);
        // Music wallpaper: on its own one-second clock (the timeout branch below does not run while messages keep arriving).
        if(normalSession&&GetTickCount64()-wallpaperCheck>=1000){wallpaperCheck=GetTickCount64();
            const auto revision=liquid::settings::ReadRevision(settingsFile);
            if(revision!=wallpaperRevision){wallpaperRevision=revision;const auto current=liquid::settings::Load(settingsFile);
                if(current.status==liquid::settings::LoadStatus::Loaded||current.status==liquid::settings::LoadStatus::Missing){
                    ApplyShortcuts(current.value.controlCenter,shortcutsActive);
                    const bool next=current.value.controlCenter.musicWallpaper;if(next!=musicWallpaper){musicWallpaper=next;WallpaperLog(next?"setting on":"setting off");if(!next)SyncMusicWallpaper(false);}}}
            if(musicWallpaper)SyncMusicWallpaper(true);}
        if(wait>=WAIT_ABANDONED_0&&wait<WAIT_ABANDONED_0+count)wait=WAIT_OBJECT_0+(wait-WAIT_ABANDONED_0);  // lease of a crashed Settings
        if(wait==WAIT_OBJECT_0)break;
        if(wait==WAIT_OBJECT_0+count){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
                if(msg.message==WM_HOTKEY&&msg.wParam==kHotkeyId){if(!suspension.Suspended())act(2,L"hotkey");}
                else if(msg.message==WM_HOTKEY&&msg.wParam>=WPARAM(kShortcutBase)&&msg.wParam<WPARAM(kShortcutBase+4))RunShortcut(int(msg.wParam-kShortcutBase),settingsFile);
                else{TranslateMessage(&msg);DispatchMessageW(&msg);}}continue;}
        if(leaseChangedIndex!=MAXDWORD&&wait==WAIT_OBJECT_0+leaseChangedIndex){
            if(suspension.OnChanged()){  // a Settings window started recording: pause the shortcut before acknowledging
                if(hotkeyRegistered){UnregisterHotKey(nullptr,kHotkeyId);hotkeyRegistered=false;}
                MSG drop{};while(PeekMessageW(&drop,nullptr,WM_HOTKEY,WM_HOTKEY,PM_REMOVE)){}  // queued presses never toggle
                suspension.Acknowledge();status.hotkey=L"suspended";publish(L"Shortcut paused while Settings records a new one.");}
            continue;}
        if(leaseIndex!=MAXDWORD&&wait==WAIT_OBJECT_0+leaseIndex){  // recording finished, cancelled, timed out or crashed
            suspension.OnLeaseEnded();reloadHotkey();publish(L"");continue;}
        bool command=false;for(int c=0;c<3;++c)if(cmdIndex[c]!=MAXDWORD&&wait==WAIT_OBJECT_0+cmdIndex[c]){act(c,L"settings");command=true;}
        if(command)continue;
        if(guardianIndex!=MAXDWORD&&wait==WAIT_OBJECT_0+guardianIndex){DWORD code=0;GetExitCodeProcess(guardian.process,&code);guardian.Close();
            guardian.error=L"guardian exited unexpectedly (code "+std::to_wstring(code)+L")";std::wstring why;
            if(taskbar.Current()==tb::State::Hidden)taskbar.Show(why);  // never stay hidden without a guardian
            publish(L"Recovery guardian stopped; taskbar restored and hiding disabled.");continue;}
        if(wait==WAIT_TIMEOUT){const auto revision=liquid::settings::ReadRevision(settingsFile);
            if(revision!=settingsRevision){settingsRevision=revision;const auto current=liquid::settings::Load(settingsFile);std::printf("manager: shared settings changed status=%d\n",static_cast<int>(current.status));std::fflush(stdout);
                if(current.status==liquid::settings::LoadStatus::Loaded||current.status==liquid::settings::LoadStatus::Missing){const auto& next=current.value.taskbar;
                    if(suspension.Suspended()){const bool keep=taskbarSettings.hideOnStartup;taskbarSettings=next;taskbarSettings.hideOnStartup=keep;}  // applied when the lease ends
                    else if(next.hotkeyEnabled!=taskbarSettings.hotkeyEnabled||next.hotkeyModifiers!=taskbarSettings.hotkeyModifiers||next.hotkeyKey!=taskbarSettings.hotkeyKey){status.message.clear();ApplyHotkey(next,status,hotkeyRegistered);taskbarSettings=next;publish(L"");}
                    taskbarSettings.hideOnStartup=next.hideOnStartup;  // preference only: the current state is not changed
                    if(next.freeWorkArea!=taskbar.FreeWorkArea()){taskbarSettings.freeWorkArea=next.freeWorkArea;taskbar.SetFreeWorkArea(next.freeWorkArea);
                        publish(next.freeWorkArea?L"Work area release enabled.":L"Work area release disabled; the reserved area was restored.");}
                    if(current.value.menuBar.enabled!=menuBarEnabled){menuBarEnabled=current.value.menuBar.enabled;
                        if(menuBarEnabled){if(!menuChild)startMenuBar();}else stopMenuBar();}}}
            std::wstring log;if(taskbar.Poll(log))publish(std::wstring(L"Taskbar state changed: ")+tb::StateName(taskbar.Current())+L".");
            if(!log.empty()){std::printf("%ls",log.c_str());std::fflush(stdout);}
            continue;}
        if(readyIndex!=MAXDWORD && wait==WAIT_OBJECT_0+readyIndex){
            const bool alive=child && WaitForSingleObject(child,0)==WAIT_TIMEOUT;
            std::printf("manager: Dock first successful Present %s\n",alive?"acknowledged":"ignored (Dock exited)");std::fflush(stdout);
            CloseHandle(firstFrameEvent);firstFrameEvent=nullptr;
            if(alive&&!guardSimDir.empty()){
                const auto guardExe=AdjacentExe(L"docky-taskbar-guard-sim.exe");
                const bool started=MatchingBuild(guardExe)&&guardSim.Start(child,single,guardSimDir,guardSimCrash);
                std::printf("guard-sim: Manager integration started=%d\n",started);std::fflush(stdout);
            }
            if(alive&&guardSimDir.empty())startGuardian();
            continue;
        }
        if(guardIndex!=MAXDWORD && wait==WAIT_OBJECT_0+guardIndex){
            const bool recovered=guardSim.RecoverAfterGuardExit();std::printf("guard-sim: guard exited, simulated recovery=%d\n",recovered);std::fflush(stdout);continue;
        }
        if(settingsIndex!=MAXDWORD && wait==WAIT_OBJECT_0+settingsIndex){
            if(settingsChild && WaitForSingleObject(settingsChild,0)==WAIT_OBJECT_0){CloseHandle(settingsChild);settingsChild=nullptr;}
            if(settingsChild){HWND h=FindOwned(settingsChild,L"LiquidDesktopSettingsV01");
                if(h){ShowWindow(h,SW_RESTORE);if(!SetForegroundWindow(h)){FLASHWINFO f{sizeof(f),h,FLASHW_TRAY|FLASHW_TIMERNOFG,3,0};FlashWindowEx(&f);}}continue;}
            auto path=AdjacentExe(L"LiquidDesktopSettings.exe");
            if(!MatchingBuild(path)){StartupError(L"Settings is missing or from a different build. Rebuild DockOver.");continue;}
            PROCESS_INFORMATION pi{};ComponentSpec spec{L"Settings",path,L"\""+path+L"\""};
            if(LaunchComponent(spec,pi)){settingsChild=pi.hProcess;CloseHandle(pi.hThread);std::printf("manager: launched Settings pid=%lu\n",pi.dwProcessId);std::fflush(stdout);}
            else StartupError(L"Could not open DockOver Settings.");
            continue;
        }
        if(quitIndex!=MAXDWORD && wait==WAIT_OBJECT_0+quitIndex){
            guardSim.Stop(); // simulation recovery precedes managed component shutdown
            {std::wstring why;if(taskbar.Current()==tb::State::Hidden&&!taskbar.Show(why))std::printf("taskbar: restore on quit failed: %ls\n",why.c_str());}
            const int dockPosted=CloseOwned(child,L"LiquidDockV01"),menuPosted=CloseOwned(menuChild,L"LiquidDesktopMenuBarV01");
            CloseOwned(settingsChild,L"LiquidDesktopSettingsV01");
            std::printf("manager: quit requested; WM_CLOSE posted dock=%d menu=%d\n",dockPosted,menuPosted);std::fflush(stdout);
            HANDLE owned[3];DWORD n=0;if(child)owned[n++]=child;if(menuChild)owned[n++]=menuChild;if(settingsChild)owned[n++]=settingsChild;
            if(n){const DWORD r=WaitForMultipleObjects(n,owned,TRUE,8000);std::printf("manager: owned components %s\n",r==WAIT_TIMEOUT?"still running after 8 s (left untouched)":"exited");}
            break;
        }
        if(child && wait==WAIT_OBJECT_0+dockIndex){DWORD code=0;if(GetExitCodeProcess(child,&code))std::printf("manager: owned Dock exited code=%lu\n",code);else std::printf("manager: Dock exit code unavailable error=%lu\n",GetLastError());CloseHandle(child);child=nullptr;
            // The Dock ending (Esc / Quit Dock) ends the managed session: close the Menu Bar we launched, then exit.
            if(menuChild){const int posted=CloseOwned(menuChild,L"LiquidDesktopMenuBarV01");std::printf("manager: closing owned Menu Bar (WM_CLOSE posted=%d)\n",posted);std::fflush(stdout);
                const DWORD r=WaitForSingleObject(menuChild,8000);std::printf("manager: owned Menu Bar %s\n",r==WAIT_OBJECT_0?"exited":"still running after 8 s (left untouched)");}
            std::fflush(stdout);break;}
        if(menuChild && wait==WAIT_OBJECT_0+menuIndex){DWORD code=0;GetExitCodeProcess(menuChild,&code);std::printf("manager: owned Menu Bar exited code=%lu; Dock continues\n",code);CloseHandle(menuChild);menuChild=nullptr;std::fflush(stdout);continue;}
        std::printf("manager: wait failed error=%lu\n",GetLastError());break;
    }
    guardSim.Stop();
    if(musicWallpaper)SyncMusicWallpaper(false);
    for(int i=0;i<4;++i)if(shortcutsActive[i]>0)UnregisterHotKey(nullptr,kShortcutBase+i);
    if(updateThread.joinable()){SetEvent(updateStop);updateThread.detach();}  // a check in progress ends with the process
    // Restore on exit (normal quit, Dock exit, Ctrl+C/close, --exit-after), then release the guardian, which
    // verifies the record once more. A crash skips this and the guardian performs the recovery instead.
    {std::wstring why;if(taskbar.Current()==tb::State::Hidden&&!taskbar.Show(why))std::printf("taskbar: restore on exit failed: %ls\n",why.c_str());}
    if(hotkeyRegistered)UnregisterHotKey(nullptr,kHotkeyId);
    guardian.Stop();status.managerPid=0;status.hotkey=L"disabled";publish(L"DockOver exited; taskbar restored.");
    // Every Manager exit path owns these child handles, including timeout and console close.
    // Ask only the windows belonging to those exact child processes to close gracefully.
    HANDLE remaining[2];DWORD remainingCount=0;
    if(child && WaitForSingleObject(child,0)==WAIT_TIMEOUT){CloseOwned(child,L"LiquidDockV01");remaining[remainingCount++]=child;}
    if(menuChild && WaitForSingleObject(menuChild,0)==WAIT_TIMEOUT){CloseOwned(menuChild,L"LiquidDesktopMenuBarV01");remaining[remainingCount++]=menuChild;}
    if(remainingCount){const DWORD result=WaitForMultipleObjects(remainingCount,remaining,TRUE,8000);
        std::printf("manager: exit cleanup components %s\n",result==WAIT_OBJECT_0?"exited":"not all exited (left untouched)");std::fflush(stdout);}
    for(HANDLE e:{hideEvent,showEvent,toggleEvent})if(e)CloseHandle(e);
    if(doneEvent)SetEvent(doneEvent);
    if(settingsChild){if(WaitForSingleObject(settingsChild,0)==WAIT_TIMEOUT){CloseOwned(settingsChild,L"LiquidDesktopSettingsV01");WaitForSingleObject(settingsChild,3000);}CloseHandle(settingsChild);}
    if(settingsEvent)CloseHandle(settingsEvent);
    if(firstFrameEvent)CloseHandle(firstFrameEvent);
    if(quitEvent)CloseHandle(quitEvent);
    if(child)CloseHandle(child); // Never terminate owned or external processes on Manager exit.
    if(menuChild)CloseHandle(menuChild);
    std::puts("manager: exiting; independent processes were not terminated");std::fflush(stdout);
    SetConsoleCtrlHandler(OnConsole,FALSE);CloseHandle(stopEvent);stopEvent=nullptr;CloseHandle(single);return 0;
}
