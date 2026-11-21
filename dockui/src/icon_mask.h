#pragma once
// Dock-only squircle preparation. Input and system icon files are never modified.
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <utility>
namespace dock {
// Backing colour behind a non-square icon inside the squircle. Measured from MyDockFinder's own processed
// icon cache (cache.gpuico, dark appearance, iconmask=1, 2026-11-10): the backing is the alpha-weighted
// average colour of the icon artwork scaled to ~37% (e.g. 166,183,178 -> 60,66,65; 168 -> 62; 179,189,218 ->
// 68,72,84). Light appearance was not observable there; it mirrors the rule toward white (an estimate).
inline std::uint32_t MaskBackingColor(const std::vector<std::uint32_t>& src,bool dark){
 double r=0,g=0,b=0,a=0;
 for(auto p:src){const double pa=p>>24;if(pa<8)continue;r+=(p>>16)&255;g+=(p>>8)&255;b+=p&255;a+=pa;}
 if(a<=0)return dark?0x3e3e3eu:0xdededeu;
 // Premultiplied input: straight average = sum(premultiplied) / sum(alpha) * 255.
 const double ar=r/a*255.0,ag=g/a*255.0,ab=b/a*255.0;constexpr double k=0.37;
 auto ch=[&](double v){return static_cast<std::uint32_t>(std::clamp(dark?v*k:255.0-(255.0-v)*k,0.0,255.0)+0.5);};
 return (ch(ar)<<16)|(ch(ag)<<8)|ch(ab);
}
// Native macOS icon outline. Measured from the official macOS 27 design-resource exports Finder@4x.png and
// Apps@4x.png (identical outlines, 288 px, shape fills the canvas): half-width of the shape at normalised
// height v = k/64 from the centre (1 = edge). A superellipse does not match (n=4: up to 14.8 px off at 288 px;
// best n=4.4 still 8.6 px); this profile reproduces the exported alpha within 0.18% mean coverage. Only the
// geometry is recorded here, no Apple artwork.
inline constexpr float kNativeOutline[65]={
 1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,
 1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,1.0000f,0.9999f,0.9993f,0.9985f,0.9980f,0.9975f,0.9966f,0.9959f,0.9951f,0.9940f,
 0.9930f,0.9917f,0.9902f,0.9884f,0.9864f,0.9840f,0.9813f,0.9781f,0.9746f,0.9705f,0.9660f,0.9609f,0.9552f,0.9488f,0.9416f,0.9334f,
 0.9246f,0.9151f,0.9047f,0.8933f,0.8807f,0.8666f,0.8518f,0.8361f,0.8191f,0.7999f,0.7790f,0.7549f,0.7264f,0.6918f,0.6462f,0.5713f,
 0.4030f};
inline float NativeOutlineHalfWidth(float v){
 v=std::clamp(v,0.f,1.f)*64.f;const int i=(std::min)(63,static_cast<int>(v));const float f=v-static_cast<float>(i);
 return kNativeOutline[i]*(1.f-f)+kNativeOutline[i+1]*f;
}
// |nx|,|ny| in 0..1 from the canvas centre. Tested in both axes so the steep part near each edge stays exact.
inline bool InsideNativeOutline(float nx,float ny){return nx<=NativeOutlineHalfWidth(ny)&&ny<=NativeOutlineHalfWidth(nx);}
inline std::vector<std::uint32_t> MakeMacStyleIcon(const std::vector<std::uint32_t>& src,unsigned w,unsigned h,bool enabled,unsigned* outputSide=nullptr,bool dark=true) {
 if(outputSide)*outputSide=0;
 if(!enabled||!w||!h||w>4096||h>4096||src.size()!=static_cast<size_t>(w)*h)return src;
 const unsigned side=(std::max)(128u,(std::max)(w,h));
 if(outputSide)*outputSide=side;
 const auto alpha=[&](unsigned x,unsigned y){return src[static_cast<size_t>(y)*w+x]>>24;};
 bool any=false;for(auto p:src)if(p>>24){any=true;break;}
 if(!any)return std::vector<std::uint32_t>(static_cast<size_t>(side)*side,0);
 // Treat a full square, or an already-rounded opaque tile, as a background icon.
 // Other silhouettes are fitted within a neutral backing to keep their subject intact.
 bool tile=w==h;
 if(tile){
  const unsigned lo=(std::min)(w-1,static_cast<unsigned>(w*0.13f)),hi=w-1-lo;
  const unsigned edge=(std::min)(w-1,static_cast<unsigned>(w*0.02f)),edgeFar=w-1-edge,mid=w/2;
  int inset=0,edges=0;
  for(auto q:{std::pair<unsigned,unsigned>{lo,lo},{hi,lo},{lo,hi},{hi,hi}})if(alpha(q.first,q.second)>=220)++inset;
  for(auto q:{std::pair<unsigned,unsigned>{mid,edge},{mid,edgeFar},{edge,mid},{edgeFar,mid}})if(alpha(q.first,q.second)>=220)++edges;
  tile=inset>=3&&edges>=3;
 }
 std::vector<std::uint32_t> out(static_cast<size_t>(side)*side);
 // Catmull-Rom cubic on the premultiplied channels: keeps edges crisp where bilinear softened them (the squircle
 // fit resamples by 0.86 for non-tile icons; bilinear at that ratio visibly blurred fine detail).
 auto sample4=[&](float x,float y,float out4[4]){
  out4[0]=out4[1]=out4[2]=out4[3]=0.f;
  if(x<-.5f||y<-.5f||x>static_cast<float>(w)-.5f||y>static_cast<float>(h)-.5f)return;
  const float fx0=std::floor(x),fy0=std::floor(y);const int x0=static_cast<int>(fx0),y0=static_cast<int>(fy0);const float tx=x-fx0,ty=y-fy0;
  auto weights=[](float t,float wt[4]){const float t2=t*t,t3=t2*t;
   wt[0]=-.5f*t3+t2-.5f*t;wt[1]=1.5f*t3-2.5f*t2+1.f;wt[2]=-1.5f*t3+2.f*t2+.5f*t;wt[3]=.5f*t3-.5f*t2;};
  float wx[4],wy[4];weights(tx,wx);weights(ty,wy);
  for(int j=0;j<4;++j){const int yy=(std::clamp)(y0-1+j,0,static_cast<int>(h)-1);
   for(int i=0;i<4;++i){const int xx=(std::clamp)(x0-1+i,0,static_cast<int>(w)-1);const float k=wx[i]*wy[j];const std::uint32_t q=src[static_cast<size_t>(yy)*w+xx];
    out4[0]+=k*static_cast<float>(q>>24);out4[1]+=k*static_cast<float>((q>>16)&255);out4[2]+=k*static_cast<float>((q>>8)&255);out4[3]+=k*static_cast<float>(q&255);}}
  out4[0]=std::clamp(out4[0],0.f,255.f);for(int c=1;c<4;++c)out4[c]=std::clamp(out4[c],0.f,out4[0]);  // stay premultiplied (colour <= alpha)
 };
 const std::uint32_t backing=tile?0u:MaskBackingColor(src,dark);
 const float br=static_cast<float>((backing>>16)&255),bg=static_cast<float>((backing>>8)&255),bb=static_cast<float>(backing&255);
 const float output=static_cast<float>(side),fit=tile?1.f:0.86f;
 const float scale=output*fit/static_cast<float>((std::max)(w,h));
 const float offX=(output-w*scale)*0.5f,offY=(output-h*scale)*0.5f;
 for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
  int inside=0;for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
   float nx=std::fabs((x+(sx+0.5f)/4.f)*2.f/output-1.f),ny=std::fabs((y+(sy+0.5f)/4.f)*2.f/output-1.f);
   if(InsideNativeOutline(nx,ny))++inside;
  }
  if(!inside)continue;const float coverage=inside/16.f;
  float sx=(x+0.5f-offX)/scale-0.5f,sy=(y+0.5f-offY)/scale-0.5f;
  float v4[4];sample4(sx,sy,v4);float a=v4[0],r=v4[1],g=v4[2],b=v4[3];
  if(!tile){const float inv=1.f-a/255.f;r+=br*inv;g+=bg*inv;b+=bb*inv;a=255.f;}
  auto c=[&](float v){return static_cast<std::uint32_t>(std::clamp(v*coverage,0.f,255.f)+0.5f);};
  out[static_cast<size_t>(y)*side+x]=(c(a)<<24)|(c(r)<<16)|(c(g)<<8)|c(b);
 }
 return out;
}
}
