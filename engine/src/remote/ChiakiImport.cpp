#include "ChiakiImport.h"

#include "Base64.h"
#include "QtSettingsCodec.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bv::remote::chiaki {
namespace {

using Settings = std::map<std::string, std::u16string>;

const std::u16string* find(const Settings& s, const std::string& key) {
    const auto it = s.find(key);
    return it == s.end() ? nullptr : &it->second;
}

std::optional<std::vector<uint8_t>> bytes(const Settings& s, const std::string& key) {
    const auto* v = find(s, key);
    return v ? qt::decodeByteArray(*v) : std::nullopt;
}

uint32_t number(const Settings& s, const std::string& key, uint32_t fallback) {
    const auto* v = find(s, key);
    if (!v || v->empty()) return fallback;
    uint64_t n = 0;
    for (char16_t c : *v) {
        if (c < u'0' || c > u'9') return fallback;
        n = n * 10 + static_cast<uint64_t>(c - u'0');
        if (n > 0xFFFFFFFFull) return fallback;
    }
    return static_cast<uint32_t>(n);
}

template <std::size_t N> bool copyExact(const std::optional<std::vector<uint8_t>>& src, std::array<uint8_t, N>& dst) {
    if (!src || src->size() != N) return false;
    std::memcpy(dst.data(), src->data(), N);
    return true;
}

// chiaki-ng's array convention: "<name>/size" plus "<name>/<1..size>/<field>".
uint32_t arraySize(const Settings& s, const std::string& name) { return number(s, name + "/size", 0); }

} // namespace

std::vector<ConsoleProfile> fromSettings(const Settings& s) {
    // Manual hosts remember the address of a registered console (by MAC).
    std::vector<std::pair<std::array<uint8_t, 6>, std::string>> addresses;
    for (uint32_t i = 1, n = arraySize(s, "manual_hosts"); i <= n; ++i) {
        const std::string base = "manual_hosts/" + std::to_string(i) + "/";
        std::array<uint8_t, 6> mac{};
        const auto* host = find(s, base + "host");
        if (host && !host->empty() && copyExact(bytes(s, base + "registered_mac"), mac)) {
            addresses.emplace_back(mac, qt::toUtf8(*host));
        }
    }

    std::array<uint8_t, 8> account{};
    bool hasAccount = false;
    if (const auto* id = find(s, "settings/psn_account_id")) {
        const auto decoded = base64Decode(qt::toUtf8(*id));
        hasAccount = copyExact(decoded, account);
    }

    std::vector<ConsoleProfile> out;
    for (uint32_t i = 1, n = arraySize(s, "registered_hosts"); i <= n; ++i) {
        const std::string base = "registered_hosts/" + std::to_string(i) + "/";
        ConsoleProfile p;
        p.source = "chiaki-ng";
        if (!copyExact(bytes(s, base + "server_mac"), p.mac)) continue;
        p.hasMac = true;
        if (!copyExact(bytes(s, base + "rp_regist_key"), p.registKey)) continue;
        if (!copyExact(bytes(s, base + "rp_key"), p.rpKey)) continue;
        p.rpKeyType = number(s, base + "rp_key_type", 0);
        p.target = number(s, base + "target", 1000100);
        p.kind = p.target >= 1000000 ? ConsoleKind::PS5 : ConsoleKind::PS4;
        if (const auto* nick = find(s, base + "server_nickname")) p.nickname = qt::toUtf8(*nick);
        const auto addr = std::find_if(addresses.begin(), addresses.end(), [&](const auto& a) { return a.first == p.mac; });
        if (addr != addresses.end()) p.host = addr->second;
        if (hasAccount) {
            p.psnAccountId = account;
            p.hasAccountId = true;
        }
        if (p.isRegistered()) out.push_back(std::move(p));
    }
    return out;
}

std::vector<ConsoleProfile> fromIniText(std::string_view text) { return fromSettings(qt::parseIni(text)); }

std::vector<ConsoleProfile> fromIniFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return fromIniText(ss.str());
}

#ifdef _WIN32
namespace {

// QSettings' registry backend: arrays are subkeys, byte arrays are "@ByteArray(...)"
// strings (REG_BINARY holding UTF-16 when they contain NUL), ints may be REG_DWORD.
void readKey(HKEY key, const std::string& prefix, Settings& out, int depth) {
    if (depth > 4) return;
    DWORD values = 0, maxName = 0, maxData = 0, subkeys = 0, maxSub = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, &maxSub, nullptr, &values, &maxName, &maxData,
                         nullptr, nullptr) != ERROR_SUCCESS) {
        return;
    }
    std::vector<wchar_t> name(maxName + 2);
    std::vector<BYTE> data(maxData + 2);
    for (DWORD i = 0; i < values; ++i) {
        DWORD nameLen = static_cast<DWORD>(name.size()), dataLen = static_cast<DWORD>(data.size()), type = 0;
        if (RegEnumValueW(key, i, name.data(), &nameLen, nullptr, &type, data.data(), &dataLen) != ERROR_SUCCESS) continue;
        std::u16string value;
        switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_MULTI_SZ:
        case REG_BINARY: {
            value.assign(reinterpret_cast<const char16_t*>(data.data()), dataLen / 2);
            if (type != REG_BINARY) {
                while (!value.empty() && value.back() == u'\0') value.pop_back();
            }
            break;
        }
        case REG_DWORD: {
            DWORD v = 0;
            std::memcpy(&v, data.data(), sizeof(v));
            for (char c : std::to_string(v)) value.push_back(static_cast<char16_t>(c));
            break;
        }
        case REG_QWORD: {
            unsigned long long v = 0;
            std::memcpy(&v, data.data(), sizeof(v));
            for (char c : std::to_string(v)) value.push_back(static_cast<char16_t>(c));
            break;
        }
        default: continue;
        }
        const std::string valueName = qt::toUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(name.data()), nameLen));
        out[prefix.empty() ? valueName : prefix + "/" + valueName] = std::move(value);
    }
    std::vector<wchar_t> sub(maxSub + 2);
    for (DWORD i = 0; i < subkeys; ++i) {
        DWORD subLen = static_cast<DWORD>(sub.size());
        if (RegEnumKeyExW(key, i, sub.data(), &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue;
        HKEY child = nullptr;
        if (RegOpenKeyExW(key, sub.data(), 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        const std::string subName = qt::toUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(sub.data()), subLen));
        readKey(child, prefix.empty() ? subName : prefix + "/" + subName, out, depth + 1);
        RegCloseKey(child);
    }
}

} // namespace

std::vector<ConsoleProfile> fromRegistry() {
    std::vector<ConsoleProfile> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Chiaki", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    wchar_t app[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, app, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (std::wstring_view(app, len).rfind(L"Chiaki", 0) != 0) continue; // "Chiaki" and "Chiaki-<profile>"
        HKEY appKey = nullptr;
        if (RegOpenKeyExW(root, app, 0, KEY_READ, &appKey) != ERROR_SUCCESS) continue;
        Settings settings;
        readKey(appKey, {}, settings, 0);
        RegCloseKey(appKey);
        mergeProfiles(out, fromSettings(settings));
    }
    RegCloseKey(root);
    return out;
}
#else
std::vector<ConsoleProfile> fromRegistry() { return {}; }
#endif

} // namespace bv::remote::chiaki
