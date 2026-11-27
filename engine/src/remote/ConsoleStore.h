#pragma once

#include "ConsoleProfile.h"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace bv::remote {

// Blackvolt's own paired-console list: JSON sealed with DPAPI, written atomically.
class ConsoleStore {
public:
    explicit ConsoleStore(std::filesystem::path file) : file_(std::move(file)) {}

    bool load();
    bool save() const;

    [[nodiscard]] const std::vector<ConsoleProfile>& consoles() const { return consoles_; }
    std::vector<ConsoleProfile>& consoles() { return consoles_; }

    // Imports from Veyra and chiaki-ng installs found on this machine. Returns new consoles.
    std::size_t importFromInstalledApps();
    std::size_t import(const std::vector<ConsoleProfile>& found) { return mergeProfiles(consoles_, found); }

    bool remove(std::size_t index);

    static std::vector<uint8_t> serialize(const std::vector<ConsoleProfile>& consoles);
    static std::optional<std::vector<ConsoleProfile>> deserialize(std::span<const uint8_t> data);

private:
    std::filesystem::path file_;
    std::vector<ConsoleProfile> consoles_;
};

} // namespace bv::remote
