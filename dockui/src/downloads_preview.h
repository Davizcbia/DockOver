#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace dock::preview {
struct Image {std::vector<std::uint32_t> pixels; unsigned w=0,h=0;};
// Scale the actual nontransparent artwork, preserving its aspect and premultiplied alpha.
inline Image Fit(const Image& in,unsigned side,float fill=0.92f) {
 if(!side||!in.w||!in.h||in.pixels.size()!=static_cast<size_t>(in.w)*in.h)return {};
 unsigned x0=in.w,y0=in.h,x1=0,y1=0;bool any=false;
 for(unsigned y=0;y<in.h;++y)for(unsigned x=0;x<in.w;++x)if((in.pixels[static_cast<size_t>(y)*in.w+x]>>24)>12){any=true;x0=(std::min)(x0,x);y0=(std::min)(y0,y);x1=(std::max)(x1,x);y1=(std::max)(y1,y);}
 if(!any)return {};const unsigned w=x1-x0+1,h=y1-y0+1;const float scale=side*fill/static_cast<float>((std::max)(w,h));
 const unsigned dw=(std::max)(1u,static_cast<unsigned>(w*scale)),dh=(std::max)(1u,static_cast<unsigned>(h*scale));
 Image out{{},side,side};out.pixels.assign(static_cast<size_t>(side)*side,0);const unsigned ox=(side-dw)/2,oy=(side-dh)/2;
 for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x){unsigned sx=x0+(std::min)(w-1,static_cast<unsigned>(x/scale)),sy=y0+(std::min)(h-1,static_cast<unsigned>(y/scale));out.pixels[static_cast<size_t>(oy+y)*side+ox+x]=in.pixels[static_cast<size_t>(sy)*in.w+sx];}return out;
}
// Newest source is first; paint older files behind it in a compact rising stack.
inline Image Stack(const std::vector<Image>& images,unsigned side) {
 if(!side||images.empty())return {};Image out{{},side,side};out.pixels.assign(static_cast<size_t>(side)*side,0);
 const unsigned count=(std::min)(3u,static_cast<unsigned>(images.size())),tile=side*76/100;
 for(int i=static_cast<int>(count)-1;i>=0;--i){auto src=Fit(images[static_cast<size_t>(i)],tile,0.94f);if(src.pixels.empty())continue;
  const unsigned x=(side-tile)/2, y=side*15/100-static_cast<unsigned>(i)*side*6/100;
  for(unsigned sy=0;sy<tile;++sy)for(unsigned sx=0;sx<tile;++sx){const auto p=src.pixels[static_cast<size_t>(sy)*tile+sx];unsigned a=p>>24;if(!a)continue;auto& d=out.pixels[static_cast<size_t>(y+sy)*side+x+sx];unsigned inv=255-a;unsigned b=(p&255)+((d&255)*inv+127)/255,g=((p>>8)&255)+(((d>>8)&255)*inv+127)/255,r=((p>>16)&255)+(((d>>16)&255)*inv+127)/255,oa=a+((d>>24)*inv+127)/255;d=((std::min)(255u,oa)<<24)|((std::min)(255u,r)<<16)|((std::min)(255u,g)<<8)|(std::min)(255u,b);}
 }return out;
}
}
