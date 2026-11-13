#pragma once
#include <string>
#include <vector>
namespace liquid::status {
enum class Wifi { Unavailable, Disconnected, Connected };
enum class PowerSource { Unknown, AC, Battery };
struct PowerInfo { bool known=false,present=false,charging=false; int percent=-1; PowerSource source=PowerSource::Unknown; };
inline PowerInfo DecodePower(bool success,unsigned ac,unsigned flags,unsigned percent){
 PowerInfo p;if(!success)return p;
 p.source=ac==1?PowerSource::AC:ac==0?PowerSource::Battery:PowerSource::Unknown;
 if(flags==255)return p;p.known=true;p.present=(flags&128u)==0;
 if(p.present){p.charging=(flags&8u)!=0;p.percent=percent<=100?static_cast<int>(percent):-1;}return p;
}
struct Snapshot {
 Wifi wifi=Wifi::Unavailable;
 bool audioAvailable=false,muted=false;
 int volumePercent=0;
 std::wstring ssid,outputDevice;
 std::vector<std::wstring> wifiNetworks;
 bool wifiListAvailable=false;
 bool batteryKnown=false,batteryPresent=false,charging=false;
 PowerSource powerSource=PowerSource::Unknown;
 int batteryPercent=-1;
 bool operator==(const Snapshot&) const = default;
};
Snapshot Query();
}
