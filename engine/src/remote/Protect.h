#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bv::remote {

// Per-user data protection (DPAPI on Windows). Pairing keys never touch disk in clear.
std::optional<std::vector<uint8_t>> protectForUser(std::span<const uint8_t> plain);
std::optional<std::vector<uint8_t>> unprotectForUser(std::span<const uint8_t> sealed);

// Overwrites memory in a way the optimizer can't drop.
void secureWipe(void* data, std::size_t size);

} // namespace bv::remote
