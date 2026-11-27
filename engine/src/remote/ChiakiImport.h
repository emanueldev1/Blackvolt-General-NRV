#pragma once

#include "ConsoleProfile.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bv::remote::chiaki {

// Interop with chiaki-ng's saved pairings (QSettings). Read-only: we never write
// into chiaki-ng's settings.

// From the flat QSettings view ("registered_hosts/1/server_mac" -> value).
std::vector<ConsoleProfile> fromSettings(const std::map<std::string, std::u16string>& settings);

// From an INI file (chiaki-ng "Export settings", or a portable install).
std::vector<ConsoleProfile> fromIniText(std::string_view text);
std::vector<ConsoleProfile> fromIniFile(const std::filesystem::path& file);

// Windows: every chiaki-ng profile stored in HKCU\Software\Chiaki. Empty elsewhere.
std::vector<ConsoleProfile> fromRegistry();

} // namespace bv::remote::chiaki
