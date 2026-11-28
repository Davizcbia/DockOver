#include "icon_mask.h"
#include <cstdio>
#include <vector>
#include <set>
#include <cstdlib>
int main(){
 std::vector<std::uint32_t> square(64*64,0xff4280c0);auto orig=square;unsigned side=0;
 auto masked=dock::MakeMacStyleIcon(square,64,64,true,&side);
 if(side!=128||square!=orig||masked[0]!=0||masked[64*side+64]!=0xff4280c0||dock::MakeMacStyleIcon(square,64,64,false)!=square)return 1;
 std::set<unsigned> edge;for(auto p:masked)if((p>>24)>0&&(p>>24)<255)edge.insert(p>>24);if(edge.size()<3)return 2;
 std::vector<std::uint32_t> irregular(64*32);for(int y=5;y<27;++y)for(int x=6;x<58;++x)irregular[y*64+x]=0xffdd3300;
 auto before=irregular;auto fitted=dock::MakeMacStyleIcon(irregular,64,32,true,&side);
 if(side!=128||irregular!=before||fitted[0]!=0||fitted[64*side+64]!=0xffdd3300||fitted[64*side+5]==0)return 3;
 for(auto p:fitted){auto a=p>>24;if(((p>>16)&255)>a||((p>>8)&255)>a||(p&255)>a)return 4;}
 std::vector<std::uint32_t> already(64*64,0xff4499aa);for(int y=0;y<8;++y)for(int x=0;x<8;++x){already[y*64+x]=0;already[y*64+63-x]=0;already[(63-y)*64+x]=0;already[(63-y)*64+63-x]=0;}
 auto preserved=dock::MakeMacStyleIcon(already,64,64,true,&side);
 if(preserved[64*side+64]!=0xff4499aa || preserved[64*side+4]!=0xff4499aa)return 5;
 std::vector<std::uint32_t> empty(16*16);auto blank=dock::MakeMacStyleIcon(empty,16,16,true,&side);for(auto p:blank)if(p)return 6;
 // Backing = average artwork colour x 0.37 (dark), mirrored toward white (light). 0xdd3300 -> (82,19,0).
 if(dock::MaskBackingColor(irregular,true)!=0x521300u)return 7;
 // Plate: the blurred artwork x 0.37 plus a sheen toward white that fades from the top (5.5%) to the bottom (0).
 // A one-colour icon gives (82,19,0) under the sheen: lighter at the top, the base colour at the bottom.
 auto ch=[](std::uint32_t p,int s){return static_cast<int>((p>>s)&255);};
 const auto topPlate=fitted[2*side+64],bottomPlate=fitted[(side-3)*side+64];
 if((topPlate>>24)!=255||(bottomPlate>>24)!=255)return 8;
 if(std::abs(ch(bottomPlate,16)-82)>2||std::abs(ch(bottomPlate,8)-19)>2||ch(bottomPlate,0)>2)return 14;
 if(ch(topPlate,16)<89||ch(topPlate,16)>93||ch(topPlate,8)<29||ch(topPlate,0)<11)return 15;
 if(dock::MaskBackingColor(irregular,false)!=0xf2b4a1u)return 9;  // 255-(255-v)*0.37
 auto light=dock::MakeMacStyleIcon(irregular,64,32,true,&side,false);const auto lightBottom=light[(side-3)*side+64];
 if(std::abs(ch(lightBottom,16)-0xf2)>2||std::abs(ch(lightBottom,8)-0xb4)>2||std::abs(ch(lightBottom,0)-0xa1)>2)return 10;
 // Two bands (red over green, like WinRAR): the plate follows them, reddish at the top and greenish at the bottom.
 std::vector<std::uint32_t> bands(64*48);for(int y=4;y<44;++y)for(int x=4;x<60;++x)bands[y*64+x]=y<24?0xffe02020u:0xff20c040u;
 auto banded=dock::MakeMacStyleIcon(bands,64,48,true,&side);
 const auto bandTop=banded[4*side+64],bandBottom=banded[(side-5)*side+64];
 if(!(ch(bandTop,16)>ch(bandTop,8)+10&&ch(bandBottom,8)>ch(bandBottom,16)+10))return 16;
 // Native outline: centre, mid-edges and the flat part of each edge are inside; the exact corner is outside;
 // the outline crosses the diagonal at about 0.847 (0.83 inside, 0.86 outside).
 if(!dock::InsideNativeOutline(0,0)||!dock::InsideNativeOutline(0.99f,0)||!dock::InsideNativeOutline(0,0.99f)||!dock::InsideNativeOutline(0.4f,0.99f))return 11;
 if(dock::InsideNativeOutline(0.97f,0.97f)||dock::InsideNativeOutline(0.86f,0.86f))return 12;
 if(!dock::InsideNativeOutline(0.83f,0.83f))return 13;
 std::puts("PASS square/rounded/non-square/transparent icons, premultiplied alpha, edge coverage, blurred backing plate and sheen, native outline and disabled");return 0;
}
