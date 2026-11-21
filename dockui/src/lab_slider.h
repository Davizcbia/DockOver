#pragma once
#include <d2d1.h>
#include <algorithm>
#include "glass_params.h"

namespace dock {
// Shared native port of Lab.qml's labGlassSlider: 6 px track, 40x30 white
// resting pill, 150 ms OutCubic growth into a lens with white opacity 1-press.
struct LabSliderShape { D2D1_RECT_F track, fill, thumb; float press; };
inline LabSliderShape LabSlider(float left,float right,float cy,float value,float controlScale,float press) {
    value=std::clamp(value,0.f,1.f);press=std::clamp(press,0.f,1.f);
    const auto lens=ControlLensGlass(30.f*controlScale);
    const float cx=left+(right-left)*value,th=3.f*controlScale;
    const float kw=40.f*controlScale*(1+(lens.sliderPressWidth-1)*press);
    const float kh=30.f*controlScale*(1+(lens.sliderPressHeight-1)*press);
    return {{left,cy-th,right,cy+th},{left,cy-th,cx,cy+th},
            {cx-kw*.5f,cy-kh*.5f,cx+kw*.5f,cy+kh*.5f},press};
}
inline void DrawLabSlider(ID2D1RenderTarget* dc,ID2D1SolidColorBrush* brush,const LabSliderShape& s,bool dark,bool glassAvailable) {
    const float r=(s.track.bottom-s.track.top)*.5f;
    brush->SetColor(D2D1::ColorF(dark?0x4B4B4D:0xDADADA));dc->FillRoundedRectangle(D2D1::RoundedRect(s.track,r,r),brush);
    brush->SetColor(D2D1::ColorF(0x398AF5));dc->FillRoundedRectangle(D2D1::RoundedRect(s.fill,r,r),brush);
    if(s.press>.001f&&glassAvailable)return;
    const float kr=(s.thumb.bottom-s.thumb.top)*.5f;
    brush->SetColor(D2D1::ColorF(1,1,1,1));dc->FillRoundedRectangle(D2D1::RoundedRect(s.thumb,kr,kr),brush);
    brush->SetColor(D2D1::ColorF(0,0,0,.12f));dc->DrawRoundedRectangle(D2D1::RoundedRect(s.thumb,kr,kr),brush,.7f);
}
}
