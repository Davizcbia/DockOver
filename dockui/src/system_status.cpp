#include "system_status.h"
#include <windows.h>
#include <wlanapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
namespace liquid::status {
namespace {
std::wstring DecodeSsid(const DOT11_SSID& id){
 const int n=static_cast<int>((std::min)(id.uSSIDLength,static_cast<ULONG>(DOT11_SSID_MAX_LENGTH)));
 if(n<=0)return {};const char* raw=reinterpret_cast<const char*>(id.ucSSID);
 const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw,n,nullptr,0);if(size<=0)return {};
 std::wstring value(static_cast<size_t>(size),L' ');if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw,n,value.data(),size)!=size)return {};
 for(auto& ch:value)if(ch<32||ch==127)ch=L' ';return value;
}
}
Snapshot Query(bool listNetworks){
 Snapshot s;DWORD negotiated=0;HANDLE wlan=nullptr;
 if(WlanOpenHandle(2,nullptr,&negotiated,&wlan)==ERROR_SUCCESS){
  PWLAN_INTERFACE_INFO_LIST list=nullptr;const DWORD rc=WlanEnumInterfaces(wlan,nullptr,&list);
  if(rc==ERROR_SUCCESS&&list){
   if(list->dwNumberOfItems)s.wifi=Wifi::Disconnected;
   for(DWORD i=0;i<list->dwNumberOfItems;++i){const auto& iface=list->InterfaceInfo[i];
    if(iface.isState==wlan_interface_state_connected){s.wifi=Wifi::Connected;DWORD bytes=0;PWLAN_CONNECTION_ATTRIBUTES info=nullptr;WLAN_OPCODE_VALUE_TYPE kind{};
     if(WlanQueryInterface(wlan,&iface.InterfaceGuid,wlan_intf_opcode_current_connection,nullptr,&bytes,reinterpret_cast<PVOID*>(&info),&kind)==ERROR_SUCCESS&&info){
      if(s.ssid.empty())s.ssid=DecodeSsid(info->wlanAssociationAttributes.dot11Ssid);WlanFreeMemory(info);}}
    PWLAN_AVAILABLE_NETWORK_LIST networks=nullptr;
    if(listNetworks&&WlanGetAvailableNetworkList(wlan,&iface.InterfaceGuid,0,nullptr,&networks)==ERROR_SUCCESS&&networks){
     s.wifiListAvailable=true;for(DWORD j=0;j<networks->dwNumberOfItems && s.wifiNetworks.size()<32;++j){
      auto name=DecodeSsid(networks->Network[j].dot11Ssid);if(!name.empty()&&std::find(s.wifiNetworks.begin(),s.wifiNetworks.end(),name)==s.wifiNetworks.end())s.wifiNetworks.push_back(std::move(name));}
     WlanFreeMemory(networks);}
   }
   WlanFreeMemory(list);
  }
  WlanCloseHandle(wlan,nullptr);
 }
 HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;Microsoft::WRL::ComPtr<IMMDevice> endpoint;Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
  HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
  if(SUCCEEDED(hr))hr=enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&endpoint);
  if(SUCCEEDED(hr)){
   Microsoft::WRL::ComPtr<IPropertyStore> properties;
   if(SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ,&properties))){PROPVARIANT value;PropVariantInit(&value);
    if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt==VT_LPWSTR && value.pwszVal)s.outputDevice=value.pwszVal;
    PropVariantClear(&value);}
   hr=endpoint->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(volume.GetAddressOf()));}
  float level=0;BOOL muted=FALSE;if(SUCCEEDED(hr)&&SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))&&SUCCEEDED(volume->GetMute(&muted))){s.audioAvailable=true;s.muted=muted!=FALSE;s.volumePercent=std::clamp(static_cast<int>(level*100.0f+0.5f),0,100);}
 }
 if(SUCCEEDED(co))CoUninitialize();
 SYSTEM_POWER_STATUS power{};const bool ok=GetSystemPowerStatus(&power)!=FALSE;
 const auto decoded=DecodePower(ok,power.ACLineStatus,power.BatteryFlag,power.BatteryLifePercent);
 s.batteryKnown=decoded.known;s.batteryPresent=decoded.present;s.charging=decoded.charging;s.batteryPercent=decoded.percent;s.powerSource=decoded.source;
 return s;
}
}
