#include "custom_icons.h"
#include "finder_fit.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dock;
#define CHECK(x) do { if(!(x)){std::fwprintf(stderr,L"FAIL line %d: %ls\n",__LINE__,error.c_str());return 1;} }while(0)
// V0.2 icon editor data model: no image fixtures needed.
static bool WriteText(const std::wstring& path,const char* text){HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;DWORD n=0;const BOOL ok=WriteFile(f,text,static_cast<DWORD>(strlen(text)),&n,nullptr);CloseHandle(f);return ok&&n==strlen(text);}
static std::string ReadText(const std::wstring& path){HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return {};char buf[8192]{};DWORD n=0;ReadFile(f,buf,sizeof(buf)-1,&n,nullptr);CloseHandle(f);return std::string(buf,n);}
static int EditTests(const std::wstring& dir){
 std::wstring error;const auto cfg=dir+L"\\icons-edit-test-"+std::to_wstring(GetCurrentProcessId())+L".json";
 // 1. A version-1 file (no edits) still loads; its override survives.
 CHECK(WriteText(cfg,"{\"format\":\"LiquidDockCustomIcons\",\"version\":1,\"icons\":{\"app:v1:a\":\"C:\\\\x\\\\a.png\"}}"));
 CustomIconStore s1;CHECK(s1.Load(cfg,error));CHECK(s1.Find("app:v1:a")&&*s1.Find("app:v1:a")==L"C:\\x\\a.png");CHECK(!s1.FindEdit("app:v1:a"));
 // 2. SetEdit writes version 2 with clamped values; the image override is kept.
 IconEdit e;e.scale=9;e.offsetX=0.1;e.offsetY=-1;e.mask=0;e.shadow=0;CHECK(s1.SetEdit("app:v1:a",e,nullptr,error));
 CustomIconStore s2;CHECK(s2.Load(cfg,error));const IconEdit* got=s2.FindEdit("app:v1:a");CHECK(got&&got->scale==1.5&&got->offsetX==0.1&&got->offsetY==-0.25&&got->mask==0&&got->shadow==0);
 CHECK(s2.Find("app:v1:a")!=nullptr);CHECK(ReadText(cfg).find("\"version\": 2")!=std::string::npos);
 // 3. Unknown top-level keys and unknown edit fields are preserved on save.
 CHECK(WriteText(cfg,"{\"format\":\"LiquidDockCustomIcons\",\"version\":2,\"future\":{\"k\":1},\"icons\":{},\"edits\":{\"app:v1:b\":{\"scale\":1.2,\"rotation\":15}}}"));
 CustomIconStore s3;CHECK(s3.Load(cfg,error));CHECK(s3.FindEdit("app:v1:b")&&s3.FindEdit("app:v1:b")->scale==1.2);
 IconEdit e2=*s3.FindEdit("app:v1:b");e2.offsetX=0.05;CHECK(s3.SetEdit("app:v1:b",e2,nullptr,error));
 const std::string saved=ReadText(cfg);CHECK(saved.find("\"future\"")!=std::string::npos&&saved.find("\"rotation\"")!=std::string::npos&&saved.find("\"offsetX\": 0.05")!=std::string::npos);
 // 4. Reset (Remove) drops the image override and every edit.
 CHECK(s3.SetEdit("app:v1:b",e2,&cfg,error));CHECK(s3.Find("app:v1:b"));CHECK(s3.Remove("app:v1:b",error));CHECK(!s3.Find("app:v1:b")&&!s3.FindEdit("app:v1:b"));
 CustomIconStore s4;CHECK(s4.Load(cfg,error));CHECK(!s4.Find("app:v1:b")&&!s4.FindEdit("app:v1:b"));
 // 5. ApplyIconEdit keeps the canvas size and moves content by offset * side.
 std::vector<uint32_t> px(16*16,0u);px[8*16+8]=0xFFFFFFFFu;IconEdit mv;mv.offsetX=0.25;auto out=ApplyIconEdit(px,16,16,mv);
 CHECK(out.size()==px.size()&&out[8*16+12]==0xFFFFFFFFu&&out[8*16+8]==0u);
 IconEdit none;CHECK(ApplyIconEdit(px,16,16,none)==px);
 DeleteFileW(cfg.c_str());DeleteFileW((cfg+L".lock").c_str());
 std::puts("PASS: icon edits - v1 compatibility, v2 save with clamping, unknown fields preserved, reset removes all, offset transform");return 0;
}
int wmain(int argc,wchar_t** argv){
 if(argc==2&&std::wstring(argv[1])==L"--finder-fit"){
  std::vector<uint32_t> padded(100*100,0),full(100*100,0xff00aaff);
  for(int y=12;y<88;++y)for(int x=15;x<85;++x)padded[y*100+x]=0xff00aaff;
  const auto fit=FitFinderBody(padded,100,100);unsigned x0=100,y0=100,x1=0,y1=0;
  for(unsigned y=0;y<100;++y)for(unsigned x=0;x<100;++x)if((fit[y*100+x]>>24)>=128){x0=(std::min)(x0,x);x1=(std::max)(x1,x);y0=(std::min)(y0,y);y1=(std::max)(y1,y);}
  if(x1-x0<85||y1-y0<90||FitFinderBody(full,100,100)!=full)return 10;
  std::puts("PASS Finder normalization grows padded art and preserves full-size art");return 0;
 }
 if(argc==3&&std::wstring(argv[1])==L"--edits")return EditTests(argv[2]);
 if(argc!=2)return 2;const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(co))return 3;
 std::wstring dir=argv[1],error;const auto png=dir+L"\\red-wide.png",ico=dir+L"\\cyan-multi.ico";
 const auto id1=ApplicationItemId(L"C:\\Windows\\explorer.exe",L""),id2=ApplicationItemId(L"C:\\Windows\\System32\\notepad.exe",L"");
 const auto relativeId=ApplicationItemId(L"relative\\app.exe",L"");wchar_t cwd[32768]{};CHECK(GetCurrentDirectoryW(32768,cwd)>0);CHECK(SetCurrentDirectoryW(dir.c_str()));CHECK(relativeId==ApplicationItemId(L"relative\\app.exe",L""));CHECK(SetCurrentDirectoryW(cwd));
 CHECK(!id1.empty()&&!id2.empty()&&id1!=id2);CHECK(id1==ApplicationItemId(L"C:/WINDOWS/EXPLORER.EXE",L""));CHECK(id1!=ApplicationItemId(L"C:\\Windows\\explorer.exe",L"--different"));
 std::vector<uint32_t> pixels;UINT w=0,h=0;CHECK(LoadCustomIconPixels(png,pixels,w,h,error));CHECK(w==160&&h==160&&pixels[0]==0);auto p=pixels[80*w+80];CHECK((p>>24)==128&&((p>>16)&255)==128&&(p&0xffff)==0);
 CHECK(LoadCustomIconPixels(ico,pixels,w,h,error));CHECK(w==256&&h==256&&pixels[0]==0);p=pixels[128*w+128];CHECK((p>>24)>0&&(p>>24)<255);CHECK(((p>>16)&255)<=(p>>24)&&((p>>8)&255)<=(p>>24)&&(p&255)<=(p>>24));
 CHECK(LoadCustomIconPixels(dir+L"\\large-wide.png",pixels,w,h,error));CHECK(w==512&&h==512&&pixels[0]==0);p=pixels[256*w+256];CHECK((p>>24)==128&&((p>>8)&255)==128);
 CHECK(LoadCustomIconPixels(dir+L"\\图标.png",pixels,w,h,error));CHECK(w==32&&h==32);
 CHECK(!LoadCustomIconPixels(dir+L"\\broken.png",pixels,w,h,error));CHECK(!LoadCustomIconPixels(dir+L"\\wrong.ico",pixels,w,h,error));
 CHECK(!LoadCustomIconPixels(dir+L"\\missing.png",pixels,w,h,error));
 const auto config=dir+L"\\icons-test-"+std::to_wstring(GetCurrentProcessId())+L".json";
 CustomIconStore store;CHECK(store.Load(config,error));CHECK(store.Set(id1,png,error));CHECK(store.Set(id2,ico,error));CHECK(store.Set("folder:v1:future",L"C:\\future\\icon.png",error));CustomIconStore reload;CHECK(reload.Load(config,error));CHECK(reload.Find(id1)&&*reload.Find(id1)==png&&reload.Find(id2)&&*reload.Find(id2)==ico);
 // A stale process must not silently overwrite another writer's saved mappings.
 CustomIconStore stale;CHECK(stale.Load(config,error));CHECK(reload.Remove(id1,error));CHECK(!stale.Set(id1,png,error));
CustomIconStore again;CHECK(again.Load(config,error));CHECK(!again.Find(id1)&&again.Find(id2)&&*again.Find(id2)==ico);
 const auto lockPath=config+L".lock";HANDLE held=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(held!=INVALID_HANDLE_VALUE);CHECK(!again.Remove(id2,error));CloseHandle(held);CHECK(again.Find(id2)&&*again.Find(id2)==ico);CHECK(again.Find("folder:v1:future")&&*again.Find("folder:v1:future")==L"C:\\future\\icon.png");
 const auto corrupt=dir+L"\\corrupt-"+std::to_wstring(GetCurrentProcessId())+L".json";HANDLE f=CreateFileW(corrupt.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);const char text[]="{broken";DWORD written=0;CHECK(WriteFile(f,text,sizeof(text)-1,&written,nullptr));CloseHandle(f);CustomIconStore bad;CHECK(!bad.Load(corrupt,error));CHECK(!bad.Set(id1,png,error));f=CreateFileW(corrupt.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);char buf[16]{};DWORD got=0;CHECK(ReadFile(f,buf,sizeof(buf),&got,nullptr));CloseHandle(f);CHECK(got==sizeof(text)-1&&std::string(buf,got)=="{broken");
 std::puts("PASS: stable IDs, PNG premultiplied alpha/aspect/large/unicode, multiframe ICO, corrupt/spoofed files, missing-file rejection, two mappings, reload/reset/type isolation, stale writer and held lock rejected, corrupt settings preserved");CoUninitialize();return 0;
}
