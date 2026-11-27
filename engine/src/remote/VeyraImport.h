#pragma once

#include "ConsoleProfile.h"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace bv::remote::veyra {

// Interop with Veyra's Remote Play pairings: one DPAPI-sealed file per console in
// %LOCALAPPDATA%\Veyra\remoteplay\<id>.dat. Read-only.

// Parses one decrypted record ("VRP" v1/v2). nullopt when malformed.
std::optional<ConsoleProfile> parseRecord(std::span<const uint8_t> plain);

// Decrypts (current Windows user) and parses every record in `dir`.
std::vector<ConsoleProfile> fromDirectory(const std::filesystem::path& dir);

// %LOCALAPPDATA%\Veyra\remoteplay, or empty when unknown.
std::filesystem::path defaultDirectory();

} // namespace bv::remote::veyra
