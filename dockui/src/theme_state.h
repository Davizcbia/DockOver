#pragma once
#include "design_tokens.h"
#include <windows.h>
namespace liquid::theme {
inline bool IsDark(int mode,bool systemDark){return mode==2||(mode==0&&systemDark);}
inline unsigned SystemRgb(int index){const COLORREF c=GetSysColor(index);return (GetRValue(c)<<16)|(GetGValue(c)<<8)|GetBValue(c);}
inline COLORREF ColorRef(unsigned rgb){return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);}
inline design::Palette Palette(int mode,bool systemDark,bool contrast){
 auto p=IsDark(mode,systemDark)?design::Dark:design::Light;
 if(contrast){p.background=p.navigation=p.surface=SystemRgb(COLOR_WINDOW);p.primary=p.secondary=p.icon=p.divider=SystemRgb(COLOR_WINDOWTEXT);
  p.accent=p.focused=SystemRgb(COLOR_HIGHLIGHT);p.selected=p.background;p.onAccent=SystemRgb(COLOR_HIGHLIGHTTEXT);p.hover=p.pressed=p.background;p.switchOff=p.secondary;}
 return p;
}
}
