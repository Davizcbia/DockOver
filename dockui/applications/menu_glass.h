#pragma once
#include "material.h"
#include "desktop/duplication.h"
#include <QImage>
#include <QRectF>
#include <QString>

// The preview owns its textures. Only the material, settings reader and capture
// implementation are shared with the menu executable.
class MenuGlass {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> source, output, staging;
    Ptr<ID3D11RenderTargetView> target;
    std::unique_ptr<dock::IPlateMaterial> material;
    docky::desk::DesktopDuplication capture;
    SIZE allocated{};
    RECT previous{};
    bool capturing = false;

    bool allocate(UINT w, UINT h) {
        if (allocated.cx == LONG(w) && allocated.cy == LONG(h)) return true;
        allocated = {};
        target.Reset(); output.Reset(); staging.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width=w; d.Height=h; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&d,nullptr,&output)) ||
            FAILED(device->CreateRenderTargetView(output.Get(),nullptr,&target))) return false;
        d.BindFlags=0; d.Usage=D3D11_USAGE_STAGING; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&d,nullptr,&staging))) return false;
        allocated={LONG(w),LONG(h)};
        return true;
    }

    QImage render(const dock::BackdropFrame& frame, RECT viewport, QRectF panel, QRectF search, float scale, float radius) {
        const UINT w=viewport.right-viewport.left, h=viewport.bottom-viewport.top;
        RECT visible{};
        if (!w || !h || !IntersectRect(&visible,&viewport,&frame.screenRect) || !allocate(w,h)) return {};
        const float black[4]={0,0,0,1};
        context->ClearRenderTargetView(target.Get(),black);
        const UINT x=visible.left-frame.screenRect.left, y=visible.top-frame.screenRect.top;
        D3D11_BOX box{x,y,0,x+UINT(visible.right-visible.left),y+UINT(visible.bottom-visible.top),1};
        context->CopySubresourceRegion(output.Get(),0,visible.left-viewport.left,visible.top-viewport.top,0,frame.texture,0,&box);
        auto params=surfaces[1];
        params.radius=radius;
        dock::GpuPaint paint;
        paint.context=context.Get(); paint.target=target.Get(); paint.targetWidth=w; paint.targetHeight=h;
        paint.windowScreenRect=viewport; paint.dpiScale=scale;
        paint.plateDip={float(panel.left()),float(panel.top()),float(panel.right()),float(panel.bottom())};
        paint.backdrop=&frame; paint.params=&params;
        if (!material->RenderGpu(paint)) return {};
        params=surfaces[0];params.radius=radius;
        paint.plateDip={float(search.left()),float(search.top()),float(search.right()),float(search.bottom())};
        if (!material->RenderGpu(paint)) return {};
        context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return {};
        QImage result=QImage(static_cast<const uchar*>(mapped.pData),w,h,mapped.RowPitch,QImage::Format_RGB32).copy();
        context->Unmap(staging.Get(),0);
        return result;
    }
public:
    dock::GlassParameters surfaces[2];
    bool forceNext=false;
    bool init() {
        HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
        if (FAILED(hr)) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
        if (FAILED(hr)) return false;
        material=dock::CreateLiquidGlassMaterial();
        if (FAILED(material->CreateGpuResources(device.Get()))) {material.reset(); return false;}
        const QString local=qEnvironmentVariable("LOCALAPPDATA");
        if (!local.isEmpty()) dock::GlassSettingsStore::Get().LoadReadOnly((local+"/LiquidDock/glass.json").toStdWString());
        return true;
    }
    QImage still(const QImage& wall, QRectF panel, QRectF search) {
        if (!material) return {};
        QImage bg=wall.convertToFormat(QImage::Format_RGB32);
        D3D11_TEXTURE2D_DESC d{}; d.Width=bg.width(); d.Height=bg.height(); d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA pixels{bg.constBits(),UINT(bg.bytesPerLine()),0};
        source.Reset();
        if (FAILED(device->CreateTexture2D(&d,&pixels,&source))) return {};
        RECT area{0,0,bg.width(),bg.height()};
        dock::BackdropFrame frame{source.Get(),area,1,0};
        return render(frame,area,panel,search,1,86);
    }
    QImage live(HWND hwnd, RECT area, QRectF panel, QRectF search, float dpi, float radius) {
        if (!material) return {};
        const bool moved=!EqualRect(&previous,&area);
        if (!capturing || moved) {
            capturing=capture.Start(device.Get(),area);
            if (!capturing) return {};
            previous=area;
        }
        // The capture excludes this window. Keep changes underneath it eligible.
        const bool changed=capture.Poll(area,0);
        dock::BackdropFrame frame;
        if ((!forceNext && !changed && !moved) || !capture.Frame(frame)) return {};
        forceNext=false;
        return render(frame,area,panel,search,dpi,radius);
    }
};
