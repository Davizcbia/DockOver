#include "workshop.h"
#include "json_lite.h"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <set>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace docky::workshop { namespace {
constexpr size_t kArchiveLimit=8u*1024u*1024u,kFileLimit=2u*1024u*1024u,kCountLimit=64;
struct Entry{std::string name;std::vector<unsigned char> bytes;uint32_t crc=0,offset=0;};
struct Archive{Pack pack;std::vector<Entry> entries;};
uint16_t u16(const std::vector<unsigned char>& b,size_t p){return uint16_t(b[p])|(uint16_t(b[p+1])<<8);}
uint32_t u32(const std::vector<unsigned char>& b,size_t p){return uint32_t(u16(b,p))|(uint32_t(u16(b,p+2))<<16);}
uint32_t Crc(const std::vector<unsigned char>& b){uint32_t c=~0u;for(auto v:b){c^=v;for(int i=0;i<8;++i)c=(c>>1)^(0xedb88320u&-(c&1u));}return ~c;}
bool Reserved(const std::string& s){if(s=="con"||s=="prn"||s=="aux"||s=="nul")return true;if(s.size()==4&&(s.rfind("com",0)==0||s.rfind("lpt",0)==0)&&s[3]>='1'&&s[3]<='9')return true;return false;}
bool Id(const std::string& s){if(s.empty()||s.size()>64||s[0]=='.'||Reserved(s))return false;for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;return true;}
bool Path(const std::string& s){if(s=="manifest.json")return true;const size_t slash=s.find('/');if(slash==std::string::npos||s.find('/',slash+1)!=std::string::npos)return false;
 if(s.substr(0,slash)!="icons"&&s.substr(0,slash)!="previews")return false;const auto n=s.substr(slash+1);if(n.empty()||n.size()>100||n[0]=='.'||n.size()<5||n.substr(n.size()-4)!=".png"||Reserved(n.substr(0,n.find('.'))))return false;
 for(unsigned char c:n)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return false;return n.find("..") == std::string::npos;}
std::wstring W(const std::string& s){return dock::json::FromUtf8(s);}
bool Directory(const std::wstring& path){DWORD a=GetFileAttributesW(path.c_str());if(a==INVALID_FILE_ATTRIBUTES){if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;a=GetFileAttributesW(path.c_str());}return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
bool Init(const std::wstring& root){if(root.empty())return false;auto parent=std::filesystem::path(root).parent_path().wstring();return Directory(parent)&&Directory(root)&&Directory(root+L"\\packs");}
bool PlainDirectory(const std::wstring& path){DWORD a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
bool Read(const std::wstring& path,std::vector<unsigned char>& out,size_t cap){out.clear();HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
 BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};bool ok=GetFileInformationByHandle(f,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(f,&size)&&size.QuadPart>=0&&static_cast<unsigned long long>(size.QuadPart)<=cap;
 if(ok){out.resize(static_cast<size_t>(size.QuadPart));DWORD got=0;ok=out.empty()||(ReadFile(f,out.data(),static_cast<DWORD>(out.size()),&got,nullptr)&&got==out.size());}CloseHandle(f);return ok;}
bool NewFile(const std::wstring& path,const std::vector<unsigned char>& bytes){HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;DWORD n=0;bool ok=(bytes.empty()||(WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)&&n==bytes.size()))&&FlushFileBuffers(f);CloseHandle(f);return ok;}
bool Png(const std::vector<unsigned char>& b,bool preview){if(b.size()<33||b.size()>kFileLimit)return false;const unsigned char sig[]={137,80,78,71,13,10,26,10};if(!std::equal(sig,sig+8,b.begin()))return false;
 ComPtr<IWICImagingFactory> f;ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;GUID format{};UINT count=0,w=0,h=0;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f));if(SUCCEEDED(hr))hr=f->CreateStream(&stream);
 if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(const_cast<BYTE*>(b.data()),static_cast<DWORD>(b.size()));if(SUCCEEDED(hr))hr=f->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetContainerFormat(&format);if(SUCCEEDED(hr))hr=decoder->GetFrameCount(&count);if(SUCCEEDED(hr)&&count==1)hr=decoder->GetFrame(0,&frame);else return false;
 if(SUCCEEDED(hr))hr=frame->GetSize(&w,&h);return SUCCEEDED(hr)&&format==GUID_ContainerFormatPng&&w>0&&h>0&&w<=(preview?2048u:1024u)&&h<=(preview?2048u:1024u);}
const Entry* Find(const Archive& a,const std::string& name){for(auto& e:a.entries)if(e.name==name)return &e;return nullptr;}
bool Parse(const std::vector<unsigned char>& b,Archive& a,std::wstring& error){a={};auto fail=[&](const wchar_t* m){error=m;return false;};
 if(b.size()<22||b.size()>kArchiveLimit)return fail(L"Archive size is invalid.");const size_t end=b.size()-22;if(u32(b,end)!=0x06054b50||u16(b,end+4)||u16(b,end+6)||u16(b,end+20))return fail(L"Unsupported ZIP ending.");
 unsigned count=u16(b,end+10);size_t cd=u32(b,end+16),cdlen=u32(b,end+12);if(!count||count>kCountLimit||u16(b,end+8)!=count||cd>end||cdlen!=end-cd)return fail(L"Invalid ZIP directory.");
 size_t pos=cd,localEnd=0,total=0;std::set<std::string> names;
 for(unsigned i=0;i<count;++i){if(pos+46>end||u32(b,pos)!=0x02014b50)return fail(L"Invalid ZIP entry.");auto flags=u16(b,pos+8),method=u16(b,pos+10);auto crc=u32(b,pos+16),cs=u32(b,pos+20),us=u32(b,pos+24);
  unsigned nl=u16(b,pos+28),el=u16(b,pos+30),cl=u16(b,pos+32);size_t off=u32(b,pos+42);if(pos+46ull+nl+el+cl>end||!nl||flags&~0x0800u||method||cs!=us||us>kFileLimit||off!=localEnd)return fail(L"Unsupported ZIP entry.");
  const uint32_t attrs=u32(b,pos+38);if(((attrs>>16)&0170000u)==0120000u)return fail(L"Links are forbidden.");std::string name(reinterpret_cast<const char*>(b.data()+pos+46),nl);if(!Path(name)||!names.insert(name).second)return fail(L"Unsafe or duplicate path.");
  if(off+30>cd||u32(b,off)!=0x04034b50||u16(b,off+6)!=flags||u16(b,off+8)!=method||u32(b,off+14)!=crc||u32(b,off+18)!=cs||u32(b,off+22)!=us)return fail(L"ZIP headers disagree.");
  const size_t lnl=u16(b,off+26),lel=u16(b,off+28),data=off+30+lnl+lel;if(data>cd||data+us>cd||lnl!=nl||std::string(reinterpret_cast<const char*>(b.data()+off+30),lnl)!=name)return fail(L"Invalid ZIP local entry.");
  Entry e;e.name=name;e.crc=crc;e.offset=static_cast<uint32_t>(off);e.bytes.assign(b.begin()+data,b.begin()+data+us);if(Crc(e.bytes)!=crc)return fail(L"ZIP checksum failed.");
  if(name!="manifest.json"&&!Png(e.bytes,name.rfind("previews/",0)==0))return fail(L"Invalid or oversized PNG.");total+=us;if(total>kArchiveLimit)return fail(L"Package content too large.");a.entries.push_back(std::move(e));localEnd=data+us;pos+=46ull+nl+el+cl;
 }
 if(pos!=end||localEnd!=cd)return fail(L"Unexpected ZIP data.");const auto* manifest=Find(a,"manifest.json");if(!manifest||manifest->bytes.size()>65536)return fail(L"Manifest missing or too large.");
 dock::json::Value v;std::string err;std::string raw(manifest->bytes.begin(),manifest->bytes.end());if(!dock::json::Parse(raw,v,err)||!v.IsObject())return fail(L"Invalid manifest.");
 const auto* fmt=v.Find("format"),*schema=v.Find("schemaVersion"),*id=v.Find("id"),*name=v.Find("name"),*version=v.Find("version"),*icons=v.Find("icons");
 if(!fmt||fmt->type!=dock::json::Value::Type::String||fmt->string!="dockypack"||!schema||schema->type!=dock::json::Value::Type::Number||schema->number!=1||!id||id->type!=dock::json::Value::Type::String||!Id(id->string)||!name||name->type!=dock::json::Value::Type::String||name->string.empty()||name->string.size()>128||!version||version->type!=dock::json::Value::Type::String||version->string.empty()||version->string.size()>32||!icons||!icons->IsObject())return fail(L"Unsupported manifest.");
 a.pack.id=id->string;a.pack.name=name->string;a.pack.version=version->string;std::set<std::string> referenced{"manifest.json"};
 const std::set<std::string> allowed{"dock.finder","dock.finder.dark","dock.launchpad","dock.launchpad.dark","dock.trash.empty","dock.trash.full","system.logo","wifi.connected","wifi.disconnected","wifi.unavailable","sound.muted","sound.low","sound.medium","sound.high","sound.unavailable","battery.charging","battery.discharging","control-center","bluetooth.on","user","input.pinyin","settings.general","settings.appearance","settings.dock","settings.menu-bar","settings.launchpad","settings.control-center","settings.taskbar","settings.workshop","settings.migration","settings.about"};
 for(const auto& [key,val]:icons->object){if(!allowed.count(key)||val.type!=dock::json::Value::Type::String||val.string.rfind("icons/",0)!=0||!Find(a,val.string)||!referenced.insert(val.string).second)return fail(L"Invalid icon mapping.");a.pack.icons.emplace(key,W(val.string));}
 if(a.pack.icons.empty())return fail(L"No supported icons.");if(const auto* preview=v.Find("preview")){if(preview->type!=dock::json::Value::Type::String||preview->string.rfind("previews/",0)!=0||!Find(a,preview->string)||!referenced.insert(preview->string).second)return fail(L"Invalid preview.");a.pack.preview=W(preview->string);}
 if(referenced.size()!=a.entries.size())return fail(L"Unreferenced files are forbidden.");return true;}
struct Lock{HANDLE h=nullptr;bool held=false;Lock(){h=CreateMutexW(nullptr,FALSE,L"Local\\docky-workshop-v1");if(h){auto w=WaitForSingleObject(h,5000);held=w==WAIT_OBJECT_0||w==WAIT_ABANDONED;}}~Lock(){if(held)ReleaseMutex(h);if(h)CloseHandle(h);}};
bool LoadInstalled(const std::wstring& root,const std::string& id,Pack& out,std::wstring& err){if(!Id(id))return false;const std::wstring base=root+L"\\packs\\"+W(id);DWORD attr=GetFileAttributesW(base.c_str());if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
 if(!PlainDirectory(base+L"\\icons")||!PlainDirectory(base+L"\\previews")){err=L"Installed pack directory is invalid.";return false;}
 std::vector<unsigned char> bytes;Archive a;if(!Read(base+L"\\package.dockypack",bytes,kArchiveLimit)||!Parse(bytes,a,err)||a.pack.id!=id)return false;
 for(auto& entry:a.entries){std::wstring rel=W(entry.name);std::replace(rel.begin(),rel.end(),L'/',L'\\');std::vector<unsigned char> copy;if(!Read(base+L"\\"+rel,copy,kFileLimit)||copy!=entry.bytes){err=L"Installed pack is damaged.";return false;}}
 out=a.pack;for(auto& [key,path]:out.icons){std::replace(path.begin(),path.end(),L'/',L'\\');path=base+L"\\"+path;}if(!out.preview.empty()){std::replace(out.preview.begin(),out.preview.end(),L'/',L'\\');out.preview=base+L"\\"+out.preview;}return true;}
bool ReadSelection(const std::wstring& root,std::string& id,bool settings=false){id.clear();std::vector<unsigned char> b;const auto path=root+(settings?L"\\settings-active.txt":L"\\active.txt");if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;if(!Read(path,b,128))return false;id.assign(b.begin(),b.end());if(!id.empty()&&id.back()=='\n')id.pop_back();return id.empty()||Id(id);}
bool WriteSelection(const std::wstring& root,const std::string& id,bool settings=false){const auto temp=root+L"\\active-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".tmp";std::string text=id+"\n";std::vector<unsigned char>b(text.begin(),text.end());if(!NewFile(temp,b))return false;bool ok=MoveFileExW(temp.c_str(),(root+(settings?L"\\settings-active.txt":L"\\active.txt")).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;if(!ok)DeleteFileW(temp.c_str());return ok;}
}
std::wstring DefaultRoot(){PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&p)))return {};std::wstring root=p;CoTaskMemFree(p);return root+L"\\docky\\workshop";}
bool Install(const std::wstring& root,const std::wstring& path,Pack& out,std::wstring& error){Lock lock;if(!lock.held||!Init(root)){error=L"Workshop unavailable or busy.";return false;}std::vector<unsigned char>b;Archive a;if(!Read(path,b,kArchiveLimit)||!Parse(b,a,error)){if(error.empty())error=L"Cannot read package.";return false;}
 const auto base=root+L"\\packs\\"+W(a.pack.id);if(GetFileAttributesW(base.c_str())!=INVALID_FILE_ATTRIBUTES){error=L"Pack already installed.";return false;}
 const auto stage=root+L"\\packs\\.stage-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());if(!CreateDirectoryW(stage.c_str(),nullptr)){error=L"Cannot create staging directory.";return false;}
 std::vector<std::wstring> made;bool ok=CreateDirectoryW((stage+L"\\icons").c_str(),nullptr)&&CreateDirectoryW((stage+L"\\previews").c_str(),nullptr);
 if(ok){auto p=stage+L"\\package.dockypack";ok=NewFile(p,b);if(ok)made.push_back(p);}
 for(const auto& e:a.entries){if(!ok)break;std::wstring rel=W(e.name);std::replace(rel.begin(),rel.end(),L'/',L'\\');auto p=stage+L"\\"+rel;ok=NewFile(p,e.bytes);if(ok)made.push_back(p);}
 if(ok)ok=MoveFileExW(stage.c_str(),base.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
 if(!ok){for(auto& p:made)DeleteFileW(p.c_str());RemoveDirectoryW((stage+L"\\icons").c_str());RemoveDirectoryW((stage+L"\\previews").c_str());RemoveDirectoryW(stage.c_str());error=L"Install failed; existing packs were left untouched.";return false;}
 return LoadInstalled(root,a.pack.id,out,error);}
bool List(const std::wstring& root,std::vector<Pack>& out,std::wstring& error){out.clear();Lock lock;if(!lock.held||!Init(root)){error=L"Workshop unavailable or busy.";return false;}WIN32_FIND_DATAW d{};HANDLE h=FindFirstFileW((root+L"\\packs\\*").c_str(),&d);if(h==INVALID_HANDLE_VALUE)return true;do{if(!(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(d.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;std::string id=dock::json::ToUtf8(d.cFileName);Pack p;std::wstring ignored;if(Id(id)){if(LoadInstalled(root,id,p,ignored))out.push_back(std::move(p));else{const auto base=root+L"\\packs\\"+W(id);std::vector<unsigned char> bytes;Archive archive;
 if(PlainDirectory(base+L"\\icons")&&PlainDirectory(base+L"\\previews")&&Read(base+L"\\package.dockypack",bytes,kArchiveLimit)&&Parse(bytes,archive,ignored)&&archive.pack.id==id){p=archive.pack;p.valid=false;p.icons.clear();p.preview.clear();out.push_back(std::move(p));}}}}while(FindNextFileW(h,&d));FindClose(h);return true;}
bool ActiveSlot(const std::wstring& root,Pack& out,bool& enabled,std::wstring& error,bool settings){enabled=false;Lock lock;if(!lock.held||!Init(root)){error=L"Workshop unavailable or busy.";return false;}std::string id;if(!ReadSelection(root,id,settings)){error=L"Selection is damaged; defaults used.";return false;}if(id.empty())return true;if(!LoadInstalled(root,id,out,error)){error=L"Selected pack is unavailable or damaged; defaults used.";return false;}enabled=true;return true;}
bool Active(const std::wstring& root,Pack& out,bool& enabled,std::wstring& error){return ActiveSlot(root,out,enabled,error,false);}
bool ActiveSettings(const std::wstring& root,Pack& out,bool& enabled,std::wstring& error){return ActiveSlot(root,out,enabled,error,true);}
bool SelectSlot(const std::wstring& root,const std::string& id,std::wstring& error,bool settings){Lock lock;if(!lock.held||!Init(root)){error=L"Workshop unavailable or busy.";return false;}std::string previous;if(!ReadSelection(root,previous,settings)){
 if(!id.empty()){error=L"Selection is damaged; not overwritten.";return false;}
 const auto path=root+(settings?L"\\settings-active.txt":L"\\active.txt");DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES||(attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){error=L"Selection is damaged; not overwritten.";return false;}
 const auto backup=root+(settings?L"\\settings-active-corrupt-":L"\\active-corrupt-")+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".bak";
 if(!CopyFileW(path.c_str(),backup.c_str(),TRUE)){error=L"Selection backup failed; not overwritten.";return false;}}
 if(!id.empty()){Pack p;if(!LoadInstalled(root,id,p,error))return false;if(settings&&std::none_of(p.icons.begin(),p.icons.end(),[](const auto& e){return e.first.rfind("settings.",0)==0;})){error=L"This pack has no Settings icons.";return false;}}
 if(!WriteSelection(root,id,settings)){error=L"Cannot save selection.";return false;}return true;}
bool Select(const std::wstring& root,const std::string& id,std::wstring& error){return SelectSlot(root,id,error,false);}
bool SelectSettings(const std::wstring& root,const std::string& id,std::wstring& error){return SelectSlot(root,id,error,true);}
bool Export(const std::wstring& root,const std::string& id,const std::wstring& destination,std::wstring& error){Lock lock;Pack p;if(!lock.held||!Init(root)||!LoadInstalled(root,id,p,error))return false;if(!CopyFileW((root+L"\\packs\\"+W(id)+L"\\package.dockypack").c_str(),destination.c_str(),TRUE)){error=L"Export failed or destination exists.";return false;}return true;}
bool Remove(const std::wstring& root,const std::string& id,std::wstring& error){Lock lock;if(!lock.held||!Init(root)||!Id(id))return false;const auto folder=root+L"\\packs\\"+W(id);
 if(!PlainDirectory(folder)||!PlainDirectory(folder+L"\\icons")||!PlainDirectory(folder+L"\\previews")){error=L"Pack directory is invalid.";return false;}std::string active,activeSettings;if(!ReadSelection(root,active)||!ReadSelection(root,activeSettings,true)){error=L"Selection is damaged; pack retained.";return false;}
 const auto base=root+L"\\packs\\"+W(id);std::vector<unsigned char>b;Archive a;if(!Read(base+L"\\package.dockypack",b,kArchiveLimit)||!Parse(b,a,error)||a.pack.id!=id)return false;
 // Refuse deletion if the pack directory contains files or directories we did not create.
 std::set<std::wstring> allowed{L".",L"..",L"icons",L"previews",L"manifest.json",L"package.dockypack"};WIN32_FIND_DATAW d{};HANDLE h=FindFirstFileW((base+L"\\*").c_str(),&d);bool clean=h!=INVALID_HANDLE_VALUE;if(clean){do{if(!allowed.count(d.cFileName)||(d.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){clean=false;break;}}while(FindNextFileW(h,&d));FindClose(h);}
 for(const wchar_t* sub:{L"icons",L"previews"}){std::set<std::wstring> expected{L".",L".."};for(const auto& e:a.entries){const std::string prefix=std::string(sub==std::wstring(L"icons")?"icons/":"previews/");if(e.name.rfind(prefix,0)==0)expected.insert(W(e.name.substr(prefix.size())));}h=FindFirstFileW((base+L"\\"+sub+L"\\*").c_str(),&d);if(h==INVALID_HANDLE_VALUE){clean=false;continue;}do{if(!expected.count(d.cFileName)||(d.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&wcscmp(d.cFileName,L".")&&wcscmp(d.cFileName,L"..")){clean=false;break;}}while(FindNextFileW(h,&d));FindClose(h);}
 if(!clean){error=L"Pack contains unexpected files; nothing removed.";return false;}if(active==id&&!WriteSelection(root,"")){error=L"Cannot disable pack; nothing removed.";return false;}if(activeSettings==id&&!WriteSelection(root,"",true)){error=L"Cannot disable Settings pack; nothing removed.";return false;}
 const auto tomb=root+L"\\packs\\.remove-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
 if(!MoveFileExW(base.c_str(),tomb.c_str(),MOVEFILE_WRITE_THROUGH)){error=L"Pack is in use; nothing removed.";return false;}
 for(const auto& e:a.entries){std::wstring rel=W(e.name);std::replace(rel.begin(),rel.end(),L'/',L'\\');if(!DeleteFileW((tomb+L"\\"+rel).c_str())){error=L"Pack removed from list; cleanup incomplete.";return false;}}
 bool ok=DeleteFileW((tomb+L"\\package.dockypack").c_str())&&RemoveDirectoryW((tomb+L"\\icons").c_str())&&RemoveDirectoryW((tomb+L"\\previews").c_str())&&RemoveDirectoryW(tomb.c_str());if(!ok)error=L"Pack removed from list; cleanup incomplete.";return ok;}
}
