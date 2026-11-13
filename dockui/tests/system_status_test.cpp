#include "system_status.h"
#include <windows.h>
#include <cstdio>
using namespace liquid::status;
int main(){
 auto ac=DecodePower(true,1,8,50);if(!ac.known||!ac.present||!ac.charging||ac.percent!=50||ac.source!=PowerSource::AC)return 4;
 auto onAC=DecodePower(true,1,0,255);if(!onAC.present||onAC.charging||onAC.percent!=-1)return 5;
 auto desktop=DecodePower(true,1,128,255);if(!desktop.known||desktop.present||desktop.percent!=-1)return 6;
 auto unknown=DecodePower(true,255,255,255);if(unknown.known||unknown.source!=PowerSource::Unknown)return 7;
 auto failed=DecodePower(false,1,8,100);if(failed.known||failed.source!=PowerSource::Unknown)return 8;
 auto battery=DecodePower(true,0,0,30);if(battery.source!=PowerSource::Battery||battery.percent!=30)return 9;
 auto s=Query();SYSTEM_POWER_STATUS p{};
 if(s.audioAvailable&&(s.volumePercent<0||s.volumePercent>100))return 1;
 if(s.batteryPresent&&(s.batteryPercent < -1 || s.batteryPercent>100))return 2;
 if(GetSystemPowerStatus(&p)&&p.BatteryFlag!=255){bool exists=(p.BatteryFlag&128)==0;if(s.batteryPresent!=exists)return 3;}
 printf("PASS status query wifi=%d audioAvailable=%d volume=%d muted=%d batteryPresent=%d batteryPercent=%d charging=%d\n",static_cast<int>(s.wifi),s.audioAvailable,s.volumePercent,s.muted,s.batteryPresent,s.batteryPercent,s.charging);return 0;}
