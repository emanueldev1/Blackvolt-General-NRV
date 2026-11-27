#include "ConsoleProfile.h"

#include <algorithm>

namespace bv::remote {
namespace {
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
template <std::size_t N> bool allZero(const std::array<uint8_t, N>& a) {
    return std::all_of(a.begin(), a.end(), [](uint8_t b) { return b == 0; });
}
} // namespace

std::string hexEncode(const uint8_t* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

std::string ConsoleProfile::macHex() const { return hexEncode(mac.data(), mac.size()); }

bool ConsoleProfile::isRegistered() const { return !allZero(registKey) && !allZero(rpKey); }

bool ConsoleProfile::sameConsole(const ConsoleProfile& o) const {
    if (hasMac && o.hasMac) return mac == o.mac;
    return !host.empty() && host == o.host;
}

std::optional<std::array<uint8_t, 6>> parseMacHex(std::string_view hex) {
    std::string clean;
    for (char c : hex) {
        if (c == ':' || c == '-' || c == ' ') continue;
        clean.push_back(c);
    }
    if (clean.size() != 12) return std::nullopt;
    std::array<uint8_t, 6> mac{};
    for (std::size_t i = 0; i < 6; ++i) {
        const int hi = nibble(clean[i * 2]), lo = nibble(clean[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        mac[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return mac;
}

std::size_t mergeProfiles(std::vector<ConsoleProfile>& into, const std::vector<ConsoleProfile>& incoming) {
    std::size_t added = 0;
    for (const auto& p : incoming) {
        if (!p.isRegistered()) continue;
        auto it = std::find_if(into.begin(), into.end(), [&](const ConsoleProfile& e) { return e.sameConsole(p); });
        if (it == into.end()) {
            into.push_back(p);
            ++added;
            continue;
        }
        // Refresh credentials (a re-pair on the other app wins) but keep what we know better.
        ConsoleProfile merged = p;
        if (merged.nickname.empty()) merged.nickname = it->nickname;
        if (merged.host.empty()) merged.host = it->host;
        if (!merged.hasMac && it->hasMac) { merged.mac = it->mac; merged.hasMac = true; }
        if (!merged.hasAccountId && it->hasAccountId) { merged.psnAccountId = it->psnAccountId; merged.hasAccountId = true; }
        if (!merged.prefs) merged.prefs = it->prefs;
        *it = std::move(merged);
    }
    return added;
}

} // namespace bv::remote
