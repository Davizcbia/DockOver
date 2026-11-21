#include "custom_icons.h"
#include "json_lite.h"
#include <bcrypt.h>
#include <wincodec.h>
#include <d3d11.h>
#include <d2d1_3.h>
#include <cstring>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace dock {
namespace {
std::wstring Message(const wchar_t* prefix, unsigned long code) {
    wchar_t buf[64]{}; swprintf_s(buf, L"0x%08lX", code); return std::wstring(prefix) + L" " + buf;
}
std::wstring NormalizeTarget(const std::wstring& target) {
    DWORD n = ExpandEnvironmentStringsW(target.c_str(), nullptr, 0);
    std::wstring expanded = target;
    if (n && n <= 32768) { std::wstring temp(n, L'\0'); if (ExpandEnvironmentStringsW(target.c_str(), temp.data(), n)) { temp.resize(n-1); expanded=std::move(temp); } }
    // Preserve relative launch specifications as written: the current working directory can vary
    // between a shortcut, Manager, and a direct launch.
    const bool driveAbsolute=expanded.size()>=3 && expanded[1]==L':' && (expanded[2]==L'\\'||expanded[2]==L'/');
    const bool uncAbsolute=expanded.size()>=2 && (expanded[0]==L'\\'||expanded[0]==L'/') && (expanded[1]==L'\\'||expanded[1]==L'/');
    if (driveAbsolute || uncAbsolute) {
        DWORD need=GetFullPathNameW(expanded.c_str(),0,nullptr,nullptr);
        if (need && need<=32768) { std::wstring full(need,L'\0'); DWORD got=GetFullPathNameW(expanded.c_str(),need,full.data(),nullptr); if(got && got<need){full.resize(got);expanded=std::move(full);} }
    }
    for (auto& c:expanded) if(c==L'/')c=L'\\';
    int count=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,expanded.data(),static_cast<int>(expanded.size()),nullptr,0,nullptr,nullptr,0);
    if(count>0){std::wstring lower(count,L'\0');if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,expanded.data(),static_cast<int>(expanded.size()),lower.data(),count,nullptr,nullptr,0))expanded=std::move(lower);}
    return expanded;
}
std::string HashSha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD size=0,cb=0;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    bool ok=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&cb,0)>=0;
    std::vector<unsigned char> object(size), digest(32);
    if(ok)ok=BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0)>=0;
    if(ok)ok=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);if(!ok)return {};
    constexpr char hex[]="0123456789abcdef";std::string out="application:v1:";
    for(auto b:digest){out.push_back(hex[b>>4]);out.push_back(hex[b&15]);}return out;
}
bool SupportedExtension(const std::wstring& path) {
    auto pos=path.find_last_of(L'.');if(pos==std::wstring::npos)return false;
    auto ext=path.substr(pos);return _wcsicmp(ext.c_str(),L".png")==0 || _wcsicmp(ext.c_str(),L".ico")==0 || _wcsicmp(ext.c_str(),L".svg")==0;
}
}
std::string ApplicationItemId(const std::wstring& target,const std::wstring& arguments) {
    if(target.empty())return {};
    const std::string t=json::ToUtf8(NormalizeTarget(target)),a=json::ToUtf8(arguments);
    if(t.empty())return {};
    return HashSha256("LiquidDock/application/v1:"+std::to_string(t.size())+":"+t+":"+std::to_string(a.size())+":"+a);
}
IconFileStamp ReadIconFileStamp(const std::wstring& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA info{};IconFileStamp stamp;
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
        stamp.exists=true;stamp.modified=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
        stamp.size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    }
    return stamp;
}
bool LoadSvgIconPixels(const std::wstring& path,std::vector<uint32_t>& pixels,UINT& width,UINT& height,std::wstring& error) {
    constexpr UINT side=512; pixels.clear();width=height=0;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow>2u*1024u*1024u){error=L"SVG file is missing, empty, or exceeds 2 MiB.";return false;}
    ComPtr<IWICImagingFactory> wf; HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wf));
    ComPtr<IWICStream> stream;if(SUCCEEDED(hr))hr=wf->CreateStream(&stream);if(SUCCEEDED(hr))hr=stream->InitializeFromFilename(path.c_str(),GENERIC_READ);
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> dc;if(SUCCEEDED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&dc);
    ComPtr<IDXGIDevice> dx;if(SUCCEEDED(hr))hr=d.As(&dx);ComPtr<ID2D1Factory1> f;if(SUCCEEDED(hr))hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),nullptr,reinterpret_cast<void**>(f.GetAddressOf()));
    ComPtr<ID2D1Device> dev;if(SUCCEEDED(hr))hr=f->CreateDevice(dx.Get(),&dev);ComPtr<ID2D1DeviceContext> ctx;if(SUCCEEDED(hr))hr=dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&ctx);ComPtr<ID2D1DeviceContext5> svgctx;if(SUCCEEDED(hr))hr=ctx.As(&svgctx);
    ComPtr<ID2D1SvgDocument> svg;if(SUCCEEDED(hr))hr=svgctx->CreateSvgDocument(stream.Get(),D2D1::SizeF(side,side),&svg);
    float sw=side,sh=side;ComPtr<ID2D1SvgElement> root;if(SUCCEEDED(hr))svg->GetRoot(&root);
    if(SUCCEEDED(hr)&&!root)hr=E_FAIL;
    if(SUCCEEDED(hr)){D2D1_SVG_LENGTH w{},h{};D2D1_SVG_VIEWBOX vb{};
        if(SUCCEEDED(root->GetAttributeValue(L"width",&w))&&SUCCEEDED(root->GetAttributeValue(L"height",&h))&&w.units==D2D1_SVG_LENGTH_UNITS_NUMBER&&h.units==D2D1_SVG_LENGTH_UNITS_NUMBER&&w.value>0&&h.value>0){sw=w.value;sh=h.value;}
        else if(SUCCEEDED(root->GetAttributeValue(L"viewBox",D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,&vb,sizeof(vb)))&&vb.width>0&&vb.height>0){sw=vb.width;sh=vb.height;}
        if(!std::isfinite(sw)||!std::isfinite(sh)||sw>100000.f||sh>100000.f)hr=E_INVALIDARG;
    }
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=side;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;if(SUCCEEDED(hr))hr=d->CreateTexture2D(&td,nullptr,&tex);ComPtr<IDXGISurface> surface;if(SUCCEEDED(hr))hr=tex.As(&surface);
    const D2D1_BITMAP_PROPERTIES1 props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    ComPtr<ID2D1Bitmap1> bitmap;if(SUCCEEDED(hr))hr=svgctx->CreateBitmapFromDxgiSurface(surface.Get(),&props,&bitmap);
    if(SUCCEEDED(hr)){const float scale=side/(std::max)(sw,sh);hr=svg->SetViewportSize(D2D1::SizeF(sw,sh));if(SUCCEEDED(hr)){svgctx->SetTarget(bitmap.Get());svgctx->BeginDraw();svgctx->Clear(D2D1::ColorF(0,0.f));svgctx->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation((side-sw*scale)*.5f,(side-sh*scale)*.5f));svgctx->DrawSvgDocument(svg.Get());hr=svgctx->EndDraw();}}
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> stage;if(SUCCEEDED(hr))hr=d->CreateTexture2D(&td,nullptr,&stage);if(SUCCEEDED(hr))dc->CopyResource(stage.Get(),tex.Get());D3D11_MAPPED_SUBRESOURCE map{};if(SUCCEEDED(hr))hr=dc->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
    if(SUCCEEDED(hr)){pixels.resize(static_cast<size_t>(side)*side);for(UINT y=0;y<side;++y)std::memcpy(pixels.data()+static_cast<size_t>(y)*side,static_cast<const BYTE*>(map.pData)+static_cast<size_t>(y)*map.RowPitch,side*4);dc->Unmap(stage.Get(),0);width=height=side;return true;}
    pixels.clear();error=Message(L"Cannot render SVG:",static_cast<unsigned long>(hr));return false;
}

bool LoadCustomIconPixels(const std::wstring& path,std::vector<uint32_t>& pixels,UINT& width,UINT& height,std::wstring& error) {
    pixels.clear();width=height=0;if(!SupportedExtension(path)){error=L"Choose a PNG, ICO, or SVG file.";return false;}
    if(path.size()>=4 && _wcsicmp(path.c_str()+path.size()-4,L".svg")==0)return LoadSvgIconPixels(path,pixels,width,height,error);
    ComPtr<IWICImagingFactory> factory;HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
    GUID container{};if(SUCCEEDED(hr))hr=decoder->GetContainerFormat(&container);
    if(SUCCEEDED(hr)){
        const bool png=_wcsicmp(path.c_str()+path.size()-4,L".png")==0;
        if((png && container!=GUID_ContainerFormatPng)||(!png && container!=GUID_ContainerFormatIco))hr=WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
    }
    UINT frames=0;if(SUCCEEDED(hr))hr=decoder->GetFrameCount(&frames);
    if(FAILED(hr)||frames==0||frames>1024){error=Message(L"Cannot decode image:",static_cast<unsigned long>(hr));return false;}
    ComPtr<IWICBitmapFrameDecode> best;UINT bestW=0,bestH=0,bestSide=0;
    for(UINT i=0;i<frames;++i){ComPtr<IWICBitmapFrameDecode> frame;UINT w=0,h=0;if(FAILED(decoder->GetFrame(i,&frame))||FAILED(frame->GetSize(&w,&h))||!w||!h||w>16384||h>16384)continue;
        UINT side=std::max(w,h);bool choose=!best || (bestSide<512 && side>bestSide) || (bestSide>=512 && side>=512 && side<bestSide);
        if(choose){best=frame;bestW=w;bestH=h;bestSide=side;}}
    if(!best){error=L"Image has no usable frame.";return false;}
    const UINT side=std::min(bestSide,512u);
    const UINT scaledW=std::max(1u,static_cast<UINT>(std::lround(double(bestW)*side/bestSide)));
    const UINT scaledH=std::max(1u,static_cast<UINT>(std::lround(double(bestH)*side/bestSide)));
    ComPtr<IWICFormatConverter> converter;hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(best.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    ComPtr<IWICBitmapSource> source=converter;ComPtr<IWICBitmapScaler> scaler;
    if(SUCCEEDED(hr) && (scaledW!=bestW||scaledH!=bestH)){hr=factory->CreateBitmapScaler(&scaler);if(SUCCEEDED(hr))hr=scaler->Initialize(converter.Get(),scaledW,scaledH,WICBitmapInterpolationModeFant);if(SUCCEEDED(hr))source=scaler;}
    if(FAILED(hr)){error=Message(L"Cannot convert image:",static_cast<unsigned long>(hr));return false;}
    pixels.assign(static_cast<size_t>(side)*side,0);const UINT x=(side-scaledW)/2,y=(side-scaledH)/2,stride=side*4;
    const UINT bytes=(scaledH-1)*stride+scaledW*4;
    hr=source->CopyPixels(nullptr,stride,bytes,reinterpret_cast<BYTE*>(pixels.data()+static_cast<size_t>(y)*side+x));
    if(FAILED(hr)){pixels.clear();error=Message(L"Cannot read image pixels:",static_cast<unsigned long>(hr));return false;}
    width=height=side;return true;
}
void IconEdit::Clamp(){
    scale=std::isfinite(scale)?std::clamp(scale,0.5,1.5):1.0;
    offsetX=std::isfinite(offsetX)?std::clamp(offsetX,-0.25,0.25):0.0;offsetY=std::isfinite(offsetY)?std::clamp(offsetY,-0.25,0.25):0.0;
    mask=std::clamp(mask,-1,1);shadow=std::clamp(shadow,-1,1);
}
bool IconEdit::IsDefault() const {return std::fabs(scale-1.0)<1e-6&&std::fabs(offsetX)<1e-6&&std::fabs(offsetY)<1e-6&&mask<0&&shadow<0&&extra.object.empty();}
std::vector<uint32_t> ApplyIconEdit(const std::vector<uint32_t>& src,UINT w,UINT h,const IconEdit& e){
    if(!w||!h||src.size()<static_cast<size_t>(w)*h)return src;
    if(std::fabs(e.scale-1.0)<1e-6&&std::fabs(e.offsetX)<1e-6&&std::fabs(e.offsetY)<1e-6)return src;
    std::vector<uint32_t> out(static_cast<size_t>(w)*h,0u);
    const double cx=w*0.5,cy=h*0.5,side=static_cast<double>((std::max)(w,h)),s=std::clamp(e.scale,0.5,1.5);
    const double ox=std::clamp(e.offsetX,-0.25,0.25)*side,oy=std::clamp(e.offsetY,-0.25,0.25)*side;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
        const double sx=(x+0.5-cx-ox)/s+cx-0.5,sy=(y+0.5-cy-oy)/s+cy-0.5;
        const int x0=static_cast<int>(std::floor(sx)),y0=static_cast<int>(std::floor(sy));const double fx=sx-x0,fy=sy-y0;
        double acc[4]{};
        for(int j=0;j<2;++j)for(int i=0;i<2;++i){const int px=x0+i,py=y0+j;if(px<0||py<0||px>=static_cast<int>(w)||py>=static_cast<int>(h))continue;
            const double wgt=(i?fx:1-fx)*(j?fy:1-fy);const uint32_t c=src[static_cast<size_t>(py)*w+px];
            for(int k=0;k<4;++k)acc[k]+=wgt*((c>>(8*k))&255u);}
        uint32_t c=0;for(int k=0;k<4;++k)c|=static_cast<uint32_t>(std::clamp(acc[k]+0.5,0.0,255.0))<<(8*k);
        out[static_cast<size_t>(y)*w+x]=c;}
    return out;
}
const IconEdit* CustomIconStore::FindEdit(const std::string& id)const{auto it=edits_.find(id);return it==edits_.end()?nullptr:&it->second;}
bool CustomIconStore::SetEdit(const std::string& id,const IconEdit& edit,const std::wstring* newImage,std::wstring& error){
    if(!available_||id.empty()){error=L"Icon settings are unavailable.";return false;}
    auto next=icons_;auto nextEdits=edits_;IconEdit e=edit;e.Clamp();
    if(newImage){if(newImage->empty())next.erase(id);else next[id]=*newImage;}
    if(e.IsDefault())nextEdits.erase(id);else nextEdits[id]=e;
    if(!Save(next,nextEdits,error))return false;icons_=std::move(next);edits_=std::move(nextEdits);return true;
}
bool CustomIconStore::Load(const std::wstring& path,std::wstring& error) {
    available_=false;path_=path;icons_.clear();edits_.clear();extraRoot_=json::Value::MakeObject();hadFile_=false;loadedBytes_.clear();
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE){DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND){available_=true;return true;}error=Message(L"Cannot open icon settings:",e);return false;}
    LARGE_INTEGER size{};if(!GetFileSizeEx(f,&size)||size.QuadPart<0||size.QuadPart>4*1024*1024){CloseHandle(f);error=L"Invalid icon settings size.";return false;}
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;BOOL ok=ReadFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr);CloseHandle(f);
    if(!ok||read!=bytes.size()){error=L"Cannot read icon settings.";return false;}
    json::Value root;std::string parseError;
    if(!json::Parse(bytes,root,parseError)||!root.IsObject()){error=L"Icon settings are corrupt; original file was preserved.";return false;}
    auto fmt=root.Find("format"),ver=root.Find("version"),icons=root.Find("icons");
    if(!fmt||fmt->type!=json::Value::Type::String||fmt->string!="LiquidDockCustomIcons"||!ver||ver->type!=json::Value::Type::Number||(ver->number!=1&&ver->number!=2)||!icons||!icons->IsObject()){error=L"Unsupported icon settings; original file was preserved.";return false;}
    std::map<std::string,std::wstring> parsed;
    for(const auto& kv:icons->object){if(kv.first.empty()||kv.second.type!=json::Value::Type::String){error=L"Invalid icon mapping; original file was preserved.";return false;}
        auto p=json::FromUtf8(kv.second.string);if(p.empty()||json::ToUtf8(p)!=kv.second.string||!parsed.emplace(kv.first,p).second){error=L"Invalid or duplicate icon mapping; original file was preserved.";return false;}}
    // Version 2: optional "edits"; unknown top-level keys and unknown edit fields are preserved.
    std::map<std::string,IconEdit> parsedEdits;
    if(auto edits=root.Find("edits")){
        if(!edits->IsObject()){error=L"Invalid icon edits; original file was preserved.";return false;}
        for(const auto& kv:edits->object){
            if(kv.first.empty()||!kv.second.IsObject()){error=L"Invalid icon edit entry; original file was preserved.";return false;}
            IconEdit e;
            for(const auto& fld:kv.second.object){
                const bool num=fld.second.type==json::Value::Type::Number;
                if(fld.first=="scale"&&num)e.scale=fld.second.number;else if(fld.first=="offsetX"&&num)e.offsetX=fld.second.number;
                else if(fld.first=="offsetY"&&num)e.offsetY=fld.second.number;else if(fld.first=="mask"&&num)e.mask=static_cast<int>(fld.second.number);
                else if(fld.first=="shadow"&&num)e.shadow=static_cast<int>(fld.second.number);else e.extra.Set(fld.first,fld.second);}
            e.Clamp();parsedEdits.emplace(kv.first,std::move(e));}
    }
    for(const auto& kv:root.object)if(kv.first!="format"&&kv.first!="version"&&kv.first!="icons"&&kv.first!="edits")extraRoot_.Set(kv.first,kv.second);
    icons_=std::move(parsed);edits_=std::move(parsedEdits);loadedBytes_=bytes;hadFile_=true;available_=true;return true;
}
std::wstring CustomIconStore::Label(const std::string& id) const {
    const auto* labels=extraRoot_.Find("labels");
    const auto* entry=labels&&labels->IsObject()?labels->Find(id):nullptr;
    return entry&&entry->type==json::Value::Type::String?json::FromUtf8(entry->string):std::wstring{};
}
bool CustomIconStore::SetLabel(const std::string& id,const std::wstring& label,std::wstring& error){
    if(!available_||id.empty()){error=L"Icon settings are unavailable.";return false;}
    const auto previous=extraRoot_;
    const auto* saved=extraRoot_.Find("labels");
    auto labels=saved&&saved->IsObject()?*saved:json::Value::MakeObject();
    labels.Set(id,json::Value::MakeString(json::ToUtf8(label)));
    extraRoot_.Set("labels",std::move(labels));
    if(Save(icons_,edits_,error))return true;
    extraRoot_=previous;
    return false;
}
const std::wstring* CustomIconStore::Find(const std::string& id)const {auto it=icons_.find(id);return it==icons_.end()?nullptr:&it->second;}
bool CustomIconStore::Set(const std::string& id,const std::wstring& path,std::wstring& error){if(!available_||id.empty()||path.empty()){error=L"Icon settings are unavailable.";return false;}auto next=icons_;next[id]=path;if(!Save(next,edits_,error))return false;icons_=std::move(next);return true;}
bool CustomIconStore::Remove(const std::string& id,std::wstring& error){if(!available_||id.empty()){error=L"Icon settings are unavailable.";return false;}auto next=icons_;auto nextEdits=edits_;const bool had=next.erase(id)>0;const bool hadEdit=nextEdits.erase(id)>0;if(!had&&!hadEdit)return true;if(!Save(next,nextEdits,error))return false;icons_=std::move(next);edits_=std::move(nextEdits);return true;}
bool CustomIconStore::Save(const std::map<std::string,std::wstring>& next,const std::map<std::string,IconEdit>& nextEdits,std::wstring& error){
    // Serialize cooperating processes and refuse to overwrite a file edited since Load/Save.
    const auto lockPath=path_+L".lock";
    HANDLE lock=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE){error=Message(L"Icon settings are busy:",GetLastError());return false;}
    struct LockGuard { HANDLE h; ~LockGuard(){CloseHandle(h);} } guard{lock};
    HANDLE current=CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(current==INVALID_HANDLE_VALUE){
        DWORD code=GetLastError();
        if(code!=ERROR_FILE_NOT_FOUND || hadFile_){error=L"Icon settings changed or became unavailable; restart Dock before editing.";return false;}
    } else {
        LARGE_INTEGER size{};
        if(!hadFile_ || !GetFileSizeEx(current,&size) || size.QuadPart<0 || size.QuadPart>4*1024*1024){CloseHandle(current);error=L"Icon settings changed externally; restart Dock before editing.";return false;}
        std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD count=0;
        const BOOL read=ReadFile(current,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr);CloseHandle(current);
        if(!read || count!=bytes.size() || bytes!=loadedBytes_){error=L"Icon settings changed externally; restart Dock before editing.";return false;}
    }
    json::Value root=json::Value::MakeObject(), icons=json::Value::MakeObject();
    const bool v2=!nextEdits.empty()||!extraRoot_.object.empty();  // plain overrides stay readable by version-1 builds
    root.Set("format",json::Value::MakeString("LiquidDockCustomIcons"));root.Set("version",json::Value::MakeNumber(v2?2:1));
    for(const auto& kv:next)icons.Set(kv.first,json::Value::MakeString(json::ToUtf8(kv.second)));root.Set("icons",std::move(icons));
    if(!nextEdits.empty()){json::Value edits=json::Value::MakeObject();
        for(const auto& [id,e]:nextEdits){json::Value o=e.extra;o.Set("scale",json::Value::MakeNumber(e.scale));o.Set("offsetX",json::Value::MakeNumber(e.offsetX));
            o.Set("offsetY",json::Value::MakeNumber(e.offsetY));o.Set("mask",json::Value::MakeNumber(e.mask));o.Set("shadow",json::Value::MakeNumber(e.shadow));edits.Set(id,std::move(o));}
        root.Set("edits",std::move(edits));}
    for(const auto& kv:extraRoot_.object)root.Set(kv.first,kv.second);
    const std::string bytes=json::Write(root);std::wstring tmp;HANDLE f=INVALID_HANDLE_VALUE;
    for(unsigned i=0;i<30&&f==INVALID_HANDLE_VALUE;++i){tmp=path_+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(i);f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_EXISTS){error=Message(L"Cannot create temporary icon settings:",GetLastError());return false;}}
    if(f==INVALID_HANDLE_VALUE){error=L"Cannot create temporary icon settings.";return false;}
    DWORD written=0;BOOL ok=WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);if(ok&&written==bytes.size())ok=FlushFileBuffers(f);else ok=FALSE;DWORD code=ok?ERROR_SUCCESS:GetLastError();
    if(!CloseHandle(f)&&ok){ok=FALSE;code=GetLastError();}if(!ok){DeleteFileW(tmp.c_str());error=Message(L"Cannot write icon settings:",code);return false;}
    if(!MoveFileExW(tmp.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){code=GetLastError();DeleteFileW(tmp.c_str());error=Message(L"Cannot save icon settings:",code);return false;}loadedBytes_=bytes;hadFile_=true;return true;
}
}
