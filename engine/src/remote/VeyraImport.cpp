#include "VeyraImport.h"

#include "Protect.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace bv::remote::veyra {
namespace {

constexpr std::size_t kHeader = 28;   // magic(3) version(1) + 6 x u32
constexpr std::size_t kSecrets = 40;  // account id 8 + regist key 16 + rp key 16
constexpr uint32_t kMaxHost = 253;

uint32_t u32le(std::span<const uint8_t> b, std::size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

} // namespace

std::optional<ConsoleProfile> parseRecord(std::span<const uint8_t> b) {
    if (b.size() < kHeader + kSecrets || b[0] != 'V' || b[1] != 'R' || b[2] != 'P') return std::nullopt;
    const uint8_t version = b[3];
    if (version != 1 && version != 2) return std::nullopt;
    const uint32_t hostSize = u32le(b, 4);
    if (hostSize == 0 || hostSize > kMaxHost) return std::nullopt;
    ConsoleProfile::StreamPrefs prefs;
    prefs.width = u32le(b, 8);
    prefs.height = u32le(b, 12);
    prefs.fps = u32le(b, 16);
    prefs.bitrateKbps = u32le(b, 20);
    prefs.codec = u32le(b, 24);
    if (prefs.codec > 2) return std::nullopt;

    std::size_t at = kHeader;
    if (b.size() < at + kSecrets + hostSize) return std::nullopt;
    ConsoleProfile p;
    p.source = "veyra";
    p.kind = ConsoleKind::PS5; // Veyra only pairs PS5 consoles
    p.target = 1000100;
    std::memcpy(p.psnAccountId.data(), &b[at], 8);
    p.hasAccountId = true;
    std::memcpy(p.registKey.data(), &b[at + 8], 16);
    std::memcpy(p.rpKey.data(), &b[at + 24], 16);
    at += kSecrets;
    p.host.assign(reinterpret_cast<const char*>(&b[at]), hostSize);
    at += hostSize;

    if (version == 2) {
        if (b.size() < at + 2) return std::nullopt;
        const uint8_t viewOnly = b[at++];
        if (viewOnly > 1) return std::nullopt;
        prefs.viewOnly = viewOnly == 1;
        const uint8_t idLen = b[at++];
        if (idLen != 0 && idLen != 12) return std::nullopt;
        if (b.size() < at + idLen) return std::nullopt;
        if (idLen == 12) {
            const auto mac = parseMacHex(std::string_view(reinterpret_cast<const char*>(&b[at]), 12));
            if (!mac) return std::nullopt;
            p.mac = *mac;
            p.hasMac = true;
        }
    }
    p.prefs = prefs;
    p.nickname = "PS5 (" + p.host + ")";
    if (!p.isRegistered()) return std::nullopt;
    return p;
}

std::vector<ConsoleProfile> fromDirectory(const std::filesystem::path& dir) {
    std::vector<ConsoleProfile> out;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".dat") continue;
        if (entry.file_size(ec) > 64 * 1024) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<uint8_t> sealed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto plain = unprotectForUser(sealed);
        if (!plain) continue;
        if (auto p = parseRecord(*plain)) mergeProfiles(out, {*p});
        secureWipe(plain->data(), plain->size());
    }
    return out;
}

std::filesystem::path defaultDirectory() {
#ifdef _WIN32
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) return std::filesystem::path(local) / L"Veyra" / L"remoteplay";
#endif
    return {};
}

} // namespace bv::remote::veyra
