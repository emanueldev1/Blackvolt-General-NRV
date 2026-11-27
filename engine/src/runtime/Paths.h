#pragma once

#include <filesystem>
#include <string>

namespace bv::runtime {

// UTF-8 <-> filesystem path without going through the ANSI code page on Windows.
inline std::filesystem::path fromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline std::string toUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

} // namespace bv::runtime
