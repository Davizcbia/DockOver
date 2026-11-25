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

// ---- Doorbell (web/dockdrop-scan/pair.js) ----------------------------------------------------------------------------
// One secret per computer names its doorbell room, which all remembered phones that can knock share; the computer then
// keeps one room open instead of one per phone. Each phone that confirmed it keeps the doorbell is flagged (REG_DWORD named
// by its id): for it the computer waits for a knock; for the others it still holds the phone's own room.
inline constexpr wchar_t kDoorbellKey[] = L"Software\\DockDrop\\DoorbellV1";

inline std::wstring DoorbellSecret() {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kDoorbellKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
            nullptr) != ERROR_SUCCESS) return {};
    std::wstring secret;
    BYTE stored[512]{};
    DWORD size = sizeof(stored), type = 0;
    if (RegQueryValueExW(key, L"secret", nullptr, &type, stored, &size) == ERROR_SUCCESS && type == REG_BINARY) {
        DATA_BLOB encoded{size, stored}, decoded{};
        if (CryptUnprotectData(&encoded, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decoded)) {
            if (decoded.cbData == 64 * sizeof(wchar_t))
                secret.assign(reinterpret_cast<const wchar_t*>(decoded.pbData), 64);
            SecureZeroMemory(decoded.pbData, decoded.cbData);
            LocalFree(decoded.pbData);
        }
    }
    if (!TrustHex(secret, 64)) {  // first use (or unreadable): a new doorbell; phones that knew an old one re-learn it
        BYTE random[32]{};
        HCRYPTPROV provider = 0;
        const bool made = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)
            && CryptGenRandom(provider, sizeof(random), random);
        if (provider) CryptReleaseContext(provider, 0);
        secret.clear();
        if (made) {
            static constexpr wchar_t digits[] = L"0123456789abcdef";
            for (BYTE b : random) { secret += digits[b >> 4]; secret += digits[b & 15]; }
            SecureZeroMemory(random, sizeof(random));
            DATA_BLOB input{static_cast<DWORD>(secret.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(secret.data())}, out{};
            if (CryptProtectData(&input, L"DockDrop doorbell", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                RegSetValueExW(key, L"secret", 0, REG_BINARY, out.pbData, out.cbData);
                LocalFree(out.pbData);
            } else secret.clear();
            // Flags from a previous doorbell are void.
            for (DWORD i = 64; i-- > 0;) {
                wchar_t name[64]{};
                DWORD chars = ARRAYSIZE(name);
                if (RegEnumValueW(key, i, name, &chars, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS
                    && TrustHex({name, chars}, 32)) RegDeleteValueW(key, name);
            }
        }
    }
    RegCloseKey(key);
    return secret;
}

inline bool PhoneKnocks(const std::wstring& id) {
    DWORD value = 0, size = sizeof(value);
    return TrustHex(id, 32) && RegGetValueW(HKEY_CURRENT_USER, kDoorbellKey, id.c_str(), RRF_RT_REG_DWORD, nullptr, &value,
        &size) == ERROR_SUCCESS && value == 1;
}

inline bool SetPhoneKnocks(const std::wstring& id) {
    if (!TrustHex(id, 32)) return false;
    const DWORD one = 1;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kDoorbellKey, id.c_str(), REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS;
}

inline bool ForgetTrustedPhone(const std::wstring& id) {
    if (!TrustHex(id, 32)) return false;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kDoorbellKey, id.c_str());
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kTrustedPhonesKey, id.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

}  // namespace docky::pairing
