#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string_view>

namespace docky::pairing {

class PhoneHost;
using PhoneMessage = std::function<void(std::wstring_view)>;
std::shared_ptr<PhoneHost> StartPhoneHost(HWND parent, PhoneMessage receive);
void ReplyToPhoneHost(const std::shared_ptr<PhoneHost>& host, const wchar_t* reply);

}  // namespace docky::pairing
