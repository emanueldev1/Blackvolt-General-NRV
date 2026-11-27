#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace bv::runtime {

// Streaming SHA-256 (FIPS 180-4). Used to verify NVIDIA runtime DLLs against the manifest.
class Sha256 {
public:
    Sha256();
    void update(std::span<const std::byte> data);
    std::array<uint8_t, 32> finish();

    static std::string hex(const std::array<uint8_t, 32>& digest);
    static std::string ofBytes(std::span<const std::byte> data);
    // Lower-case hex digest of a file, or nullopt if it cannot be read.
    static std::optional<std::string> ofFile(const std::filesystem::path& path);

private:
    void block(const uint8_t* p);

    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buf_{};
    std::size_t bufLen_ = 0;
    uint64_t totalBytes_ = 0;
};

} // namespace bv::runtime
