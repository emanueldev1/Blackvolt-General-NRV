#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bv::remote::qt {

// Reads the subset of Qt's QSettings formats that chiaki-ng uses for its pairings.
// Implemented from Qt's documented INI escaping and variant encoding rules.

// "@ByteArray(...)" (already unescaped) -> raw bytes. nullopt if not a byte array.
std::optional<std::vector<uint8_t>> decodeByteArray(std::u16string_view value);

// Unescapes one INI value (quotes, \\x.., \\0.., \\n, ...).
std::u16string unescapeIniValue(std::string_view raw);

// Flat map "section/key" -> unescaped value. Keys keep Qt's array form, e.g.
// "registered_hosts/1\\server_mac" becomes "registered_hosts/1/server_mac".
std::map<std::string, std::u16string> parseIni(std::string_view text);

std::string toUtf8(std::u16string_view s);

} // namespace bv::remote::qt
