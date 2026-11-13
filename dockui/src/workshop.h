#pragma once
#include <windows.h>
#include <map>
#include <string>
#include <vector>
namespace docky::workshop {
struct Pack { bool valid=true;std::string id,name,version;std::map<std::string,std::wstring> icons;std::wstring preview; };
std::wstring DefaultRoot();
// Operations are local-only. Archives are stored ZIPs with bounded PNG/JSON entries.
bool Install(const std::wstring& root,const std::wstring& archive,Pack& out,std::wstring& error);
bool List(const std::wstring& root,std::vector<Pack>& out,std::wstring& error);
bool Active(const std::wstring& root,Pack& out,bool& enabled,std::wstring& error);
bool Select(const std::wstring& root,const std::string& id,std::wstring& error);
bool ActiveSettings(const std::wstring& root,Pack& out,bool& enabled,std::wstring& error);
bool SelectSettings(const std::wstring& root,const std::string& id,std::wstring& error); // empty = defaults
bool Remove(const std::wstring& root,const std::string& id,std::wstring& error);
bool Export(const std::wstring& root,const std::string& id,const std::wstring& destination,std::wstring& error);
}
