#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <vector>
#include "design_tokens.h"

namespace docky::battery {
// A scalable background and separate live foreground, drawn into a premultiplied surface.
// onLight: the menu bar over a light backdrop (black ink): dark fill and light digits, the same shapes.
inline void Draw(Gdiplus::Graphics& g, float cx, float cy, float scale, int percent, bool charging, bool onLight=false) {
    using namespace Gdiplus;
    // Center the visually dominant body; the small terminal should not pull it left.
    const float w=27.f*scale, h=14.f*scale, x=cx-w*.5f, y=cy-h*.5f, r=4.f*scale;
    const int level=percent<0 ? -1 : std::clamp(percent,0,100);
    const float fill=level<0 ? 0.f : w*level/100.f;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    GraphicsPath body;
    body.AddArc(x,y,2*r,2*r,180,90); body.AddArc(x+w-2*r,y,2*r,2*r,270,90);
    body.AddArc(x+w-2*r,y+h-2*r,2*r,2*r,0,90); body.AddArc(x,y+h-2*r,2*r,2*r,90,90); body.CloseFigure();
    SolidBrush white(onLight?Color(217,0,0,0):Color(255,247,247,248)), grey(onLight?Color(255,168,168,171):Color(255,150,150,153)),
               ink(onLight?Color(255,186,186,190):Color(255,10,10,12));
    g.FillPath(&grey,&body);
    auto saved=g.Save(); g.SetClip(&body,CombineModeIntersect);
    if(fill>0.f) g.FillRectangle(&white,x,y,fill,h);
    g.Restore(saved);
    SolidBrush* terminal=level>=99 ? &white : &grey;
    const float tx=x+w+0.8f*scale, tw=1.7f*scale, th=5.f*scale;
    g.FillRectangle(terminal,tx,cy-th*.5f,tw,th);

    wchar_t value[8]{};
    if(level<0) wcscpy_s(value,L"?"); else _snwprintf_s(value,_TRUNCATE,L"%d",level);
    FontFamily family(liquid::design::type::FamilyGdi);
    GraphicsPath digits;
    digits.AddString(value,-1,&family,FontStyleBold,12.5f*scale,PointF(0,0),StringFormat::GenericTypographic());
    RectF bounds; digits.GetBounds(&bounds);
    const float boltWidth=charging?6.4f*scale:0.f, gap=charging?.6f*scale:0.f;  // larger bolt (was 4.0: hard to see)
    const float available=w-2.f*scale;
    const float factor=(std::min)(1.f,(available-boltWidth-gap)/(std::max)(bounds.Width,.01f));
    const float digitWidth=bounds.Width*factor;
    const float groupWidth=digitWidth+gap+boltWidth;
    const float left=cx-groupWidth*.5f;
    Matrix transform; transform.Scale(factor,factor); transform.Translate(left/factor-bounds.X,(cy/factor)-(bounds.Y+bounds.Height*.5f));
    digits.Transform(&transform);
    g.FillPath(&ink,&digits);
    if(charging){
      const float bx=left+digitWidth+gap+2.7f*scale,k=1.0f*scale,kx=1.6f*scale;  // digit height, bolder (wider) strokes
      PointF points[]={{bx+1.8f*kx,cy-5.2f*k},{bx-1.7f*kx,cy+.4f*k},
        {bx+.2f*kx,cy+.4f*k},{bx-.8f*kx,cy+5.2f*k},
        {bx+2.3f*kx,cy-.8f*k},{bx+.6f*kx,cy-.8f*k}};
      // Short curves soften the corners while retaining the familiar lightning shape.
      GraphicsPath rounded;
      for(int i=0;i<6;++i){
        const PointF& p=points[i];const PointF& prev=points[(i+5)%6];const PointF& next=points[(i+1)%6];
        const PointF entry(p.X+(prev.X-p.X)*.12f,p.Y+(prev.Y-p.Y)*.12f);
        const PointF exit(p.X+(next.X-p.X)*.12f,p.Y+(next.Y-p.Y)*.12f);
        rounded.AddBezier(entry,p,p,exit);
      }
      rounded.CloseFigure();g.FillPath(&ink,&rounded);
    }
}
// Render a small premultiplied surface at 4x and area-average it before composition.
// This also antialiases GDI+ clip boundaries, which otherwise snap to physical pixels.
inline void DrawSmooth(std::uint32_t* dest,int width,int height,float cx,float cy,float scale,int percent,bool charging,bool onLight=false) {
    if(!dest||width<=0||height<=0||scale<=0.f)return;
    constexpr int sub=4;
    const int x0=static_cast<int>(std::floor(cx-17.f*scale-2.f));
    const int y0=static_cast<int>(std::floor(cy-8.f*scale-2.f));
    const int x1=static_cast<int>(std::ceil(cx+18.f*scale+2.f));
    const int y1=static_cast<int>(std::ceil(cy+8.f*scale+2.f));
    const int w=x1-x0,h=y1-y0;if(w<=0||h<=0||w>4096||h>4096)return;
    std::vector<std::uint32_t> high(static_cast<size_t>(w)*h*sub*sub,0);
    {
      Gdiplus::Bitmap image(w*sub,h*sub,w*sub*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(high.data()));
      Gdiplus::Graphics g(&image);g.ScaleTransform(static_cast<float>(sub),static_cast<float>(sub));
      Draw(g,cx-x0,cy-y0,scale,percent,charging,onLight);
    }
    for(int y=0;y<h;++y){const int dy=y0+y;if(dy<0||dy>=height)continue;
      for(int x=0;x<w;++x){const int dx=x0+x;if(dx<0||dx>=width)continue;unsigned sum[4]{};
        for(int sy=0;sy<sub;++sy)for(int sx=0;sx<sub;++sx){const auto pixel=high[static_cast<size_t>(y*sub+sy)*w*sub+x*sub+sx];for(int c=0;c<4;++c)sum[c]+=(pixel>>(8*c))&255u;}
        unsigned src[4];for(int c=0;c<4;++c)src[c]=(sum[c]+sub*sub/2)/(sub*sub);
        auto& out=dest[static_cast<size_t>(dy)*width+dx];const unsigned inv=255-src[3];std::uint32_t value=0;
        for(int c=0;c<4;++c)value|=static_cast<std::uint32_t>((std::min)(255u,src[c]+(((out>>(8*c))&255u)*inv+127u)/255u))<<(8*c);
        out=value;
      }
    }
}

}
