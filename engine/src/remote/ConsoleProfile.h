#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bv::remote {

enum class ConsoleKind { PS4, PS5 };

// Everything needed to start a Remote Play session with an already paired console.
struct ConsoleProfile {
    std::string nickname;                 // e.g. "PS5-123"
    std::string host;                     // last known IP / hostname, may be empty (discovery fills it)
    std::array<uint8_t, 6> mac{};         // console id
    ConsoleKind kind = ConsoleKind::PS5;
    uint32_t target = 1000100;            // chiaki target value (PS5_1 by default)
    std::array<uint8_t, 16> registKey{};  // rp_regist_key
    std::array<uint8_t, 16> rpKey{};      // morning / rp_key
    uint32_t rpKeyType = 0;
    std::array<uint8_t, 8> psnAccountId{};
    bool hasAccountId = false;
    std::string source;                   // "veyra", "chiaki-ng", "blackvolt"
    bool hasMac = false;                  // Veyra v1 files carry no console id

    // Stream preferences, when the source stored them (Veyra does).
    struct StreamPrefs {
        uint32_t width = 0, height = 0, fps = 0, bitrateKbps = 0;
        uint32_t codec = 0; // 0 H.264, 1 HEVC, 2 HEVC HDR
        bool viewOnly = false;
    };
    std::optional<StreamPrefs> prefs;

    [[nodiscard]] bool isRegistered() const;

    [[nodiscard]] std::string macHex() const;
    // Same physical console: by console id when both know it, otherwise by host.
    [[nodiscard]] bool sameConsole(const ConsoleProfile& o) const;
};

std::optional<std::array<uint8_t, 6>> parseMacHex(std::string_view hex);
std::string hexEncode(const uint8_t* data, std::size_t size);

// Adds or refreshes profiles in `into`; returns how many were new.
std::size_t mergeProfiles(std::vector<ConsoleProfile>& into, const std::vector<ConsoleProfile>& incoming);

} // namespace bv::remote
