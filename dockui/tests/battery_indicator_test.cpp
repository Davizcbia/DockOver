#include "battery_indicator.h"
#include <cstdio>
#include <vector>
using namespace Gdiplus;
int wmain(int argc,wchar_t** argv) {
 GdiplusStartupInput input; ULONG_PTR token=0; if(GdiplusStartup(&token,&input,nullptr)!=Ok)return 1;
 int result=0;
 {
  auto render=[](int percent,bool charging){
   Bitmap image(80,40,PixelFormat32bppARGB);Graphics g(&image);g.Clear(Color(0,0,0,0));docky::battery::Draw(g,40,20,1,percent,charging);
   std::vector<ARGB> pixels;for(int y=0;y<40;++y)for(int x=0;x<80;++x){Color c;image.GetPixel(x,y,&c);pixels.push_back(c.GetValue());}return pixels;};
  auto zero=render(0,false),zeroCharging=render(0,true),half=render(50,false),full=render(100,false),charging=render(100,true),unknown=render(-1,false);
  // Top interior is clear of glyphs and follows the live fill boundary.
  auto red=[](ARGB v){return (v>>16)&255;};
  if(!(red(zero[15*80+30])<180&&red(half[15*80+30])>200&&red(half[15*80+50])<180&&red(full[15*80+50])>200))result=2;
  if(full==charging||zero==unknown||half==full)result=3;
  // At zero charge the body is grey: neither digits nor bolt may become white.
  bool darkInk=false;
  for(int y=15;y<25;++y)for(int x=28;x<52;++x){
   const auto px=zeroCharging[y*80+x];if(red(px)>180)result=5;if(red(px)<80 && (px>>24)>200)darkInk=true;
  }
  if(!darkInk)result=6;
  for(float scale:{1.f,1.5f,2.f}){
   std::vector<std::uint32_t> smooth(120*80,0);docky::battery::DrawSmooth(smooth.data(),120,80,60,40,scale,100,true);
   bool partial=false,opaque=false;for(auto pixel:smooth){const unsigned a=pixel>>24;partial|=a>0&&a<255;opaque|=a==255;}
   if(!partial||!opaque)result=8;
  }
  // Every adjacent integer must result in a distinct rendered live state.
  for(int level=0;level<100;++level)
   if(render(level,true)==render(level+1,true)||render(level,false)==render(level+1,false))result=7;
  if(argc>1){
   Bitmap preview(860,310,PixelFormat32bppARGB);Graphics g(&preview);g.Clear(Color(255,31,34,41));
   FontFamily family(L"Segoe UI");Font label(&family,16,FontStyleRegular,UnitPixel);SolidBrush white(Color(255,240,240,240));
   const int levels[]={0,9,10,80,98,100};
   for(int row=0;row<2;++row)for(int i=0;i<6;++i){
    const float x=78.f+i*138.f,y=70.f+row*130.f;docky::battery::Draw(g,x,y,3.5f,levels[i],row==1);docky::battery::Draw(g,x,y+39.f,1.f,levels[i],row==1);
    wchar_t text[32]{};if(levels[i]<0)wcscpy_s(text,L"unknown");else _snwprintf_s(text,_TRUNCATE,L"%d%%",levels[i]);
    g.DrawString(text,-1,&label,PointF(x-20,y+52),&white);
   }
   CLSID bmp{};CLSIDFromString(L"{557CF400-1A04-11D3-9A73-0000F81EF32E}",&bmp);if(preview.Save(argv[1],&bmp,nullptr)!=Ok)result=4;
  }
 }
 GdiplusShutdown(token);if(result)std::printf("FAIL battery indicator %d\n",result);else std::puts("PASS battery fill, charging and unknown state rendering");return result;
}
