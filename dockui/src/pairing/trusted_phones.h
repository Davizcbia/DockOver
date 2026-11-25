#pragma once

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace docky::pairing {

inline constexpr wchar_t kTrustedPhonesKey[] = L"Software\\DockDrop\\TrustedPhonesV1";
inline constexpr size_t kTrustedPhoneLimit = 6;

struct TrustedPhone {
    std::wstring id, secret, name;
};

inline bool TrustHex(std::wstring_view text, size_t length) {
    return text.size() == length && text.find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos;
}

inline bool TrustName(std::wstring_view text) {
    return !text.empty() && text.size() <= 64 && std::none_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch < 32 || (ch >= 127 && ch <= 159) || (ch >= 0x202a && ch <= 0x202e)
            || (ch >= 0x2066 && ch <= 0x2069);
    });
}

inline std::vector<TrustedPhone> ReadTrustedPhones() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTrustedPhonesKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    std::vector<TrustedPhone> phones;
    for (DWORD i = 0; i < 64 && phones.size() < kTrustedPhoneLimit; ++i) {
        wchar_t id[33]{};
        DWORD chars = ARRAYSIZE(id), type = 0;
        BYTE ciphertext[1024]{};
        DWORD size = sizeof(ciphertext);
        const auto result = RegEnumValueW(key, i, id, &chars, nullptr, &type, ciphertext, &size);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS || type != REG_BINARY || !TrustHex({id, chars}, 32)) continue;
        DATA_BLOB encoded{size, ciphertext}, decoded{};
        if (!CryptUnprotectData(&encoded, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decoded))
            continue;
        std::wstring value;
        if (decoded.cbData % sizeof(wchar_t) == 0 && decoded.cbData <= 128 * sizeof(wchar_t))
            value.assign(reinterpret_cast<const wchar_t*>(decoded.pbData), decoded.cbData / sizeof(wchar_t));
        SecureZeroMemory(decoded.pbData, decoded.cbData);
        LocalFree(decoded.pbData);
        if (value.size() > 64 && TrustHex(std::wstring_view(value).substr(0, 64), 64)
            && TrustName(std::wstring_view(value).substr(64)))
            phones.push_back({id, value.substr(0, 64), value.substr(64)});
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    }
    RegCloseKey(key);
    return phones;
}

inline bool SaveTrustedPhone(const TrustedPhone& phone) {
    if (!TrustHex(phone.id, 32) || !TrustHex(phone.secret, 64) || !TrustName(phone.name)
        || ReadTrustedPhones().size() >= kTrustedPhoneLimit)
        return false;
    auto plain = phone.secret + phone.name;
    DATA_BLOB input{static_cast<DWORD>(plain.size() * sizeof(wchar_t)),
        reinterpret_cast<BYTE*>(plain.data())}, encrypted{};
    const bool protectedData = CryptProtectData(&input, L"DockDrop phone", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &encrypted) != FALSE;
    SecureZeroMemory(plain.data(), plain.size() * sizeof(wchar_t));
    if (!protectedData) return false;
    HKEY key{};
    bool saved = false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTrustedPhonesKey, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const auto exists = RegQueryValueExW(key, phone.id.c_str(), nullptr, nullptr, nullptr, nullptr);
        saved = exists == ERROR_FILE_NOT_FOUND && RegSetValueExW(key, phone.id.c_str(), 0, REG_BINARY,
            encrypted.pbData, encrypted.cbData) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    LocalFree(encrypted.pbData);
    return saved;
}

inline bool ForgetTrustedPhone(const std::wstring& id) {
    if (!TrustHex(id, 32)) return false;
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kTrustedPhonesKey, id.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

}  // namespace docky::pairing
