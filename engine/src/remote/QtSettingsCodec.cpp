#include "QtSettingsCodec.h"

#include <cctype>

namespace bv::remote::qt {
namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes UTF-8 to UTF-16 (values may contain non-ASCII console names).
std::u16string fromUtf8(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        std::size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            len = 4;
        } else { cp = c; len = 1; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

} // namespace

std::string toUtf8(std::u16string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string unescapeIniValue(std::string_view raw) {
    const std::string s = trim(raw);
    std::u16string out;
    std::string pending; // UTF-8 run of literal characters
    auto flush = [&] {
        out += fromUtf8(pending);
        pending.clear();
    };
    bool inQuotes = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (c != '\\' || i + 1 >= s.size()) {
            pending.push_back(c);
            continue;
        }
        const char e = s[++i];
        char16_t value = 0;
        switch (e) {
        case 'a': value = 0x07; break;
        case 'b': value = 0x08; break;
        case 'f': value = 0x0C; break;
        case 'n': value = 0x0A; break;
        case 'r': value = 0x0D; break;
        case 't': value = 0x09; break;
        case 'v': value = 0x0B; break;
        case '"': case '\'': case '?': case '\\': value = static_cast<char16_t>(e); break;
        case 'x': {
            unsigned v = 0;
            while (i + 1 < s.size() && hexValue(s[i + 1]) >= 0) v = v * 16 + static_cast<unsigned>(hexValue(s[++i]));
            value = static_cast<char16_t>(v);
            break;
        }
        default:
            if (e >= '0' && e <= '7') {
                unsigned v = static_cast<unsigned>(e - '0');
                for (int k = 0; k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++k) {
                    v = v * 8 + static_cast<unsigned>(s[++i] - '0');
                }
                value = static_cast<char16_t>(v);
            } else {
                value = static_cast<char16_t>(static_cast<unsigned char>(e));
            }
        }
        flush();
        out.push_back(value);
    }
    flush();
    (void)inQuotes;
    return out;
}

std::optional<std::vector<uint8_t>> decodeByteArray(std::u16string_view value) {
    constexpr std::u16string_view prefix = u"@ByteArray(";
    if (value.size() < prefix.size() + 1 || value.substr(0, prefix.size()) != prefix || value.back() != u')') {
        return std::nullopt;
    }
    const auto body = value.substr(prefix.size(), value.size() - prefix.size() - 1);
    std::vector<uint8_t> bytes;
    bytes.reserve(body.size());
    for (char16_t c : body) bytes.push_back(static_cast<uint8_t>(c & 0xFF)); // Latin-1 per Qt
    return bytes;
}

std::map<std::string, std::u16string> parseIni(std::string_view text) {
    std::map<std::string, std::u16string> out;
    std::string section;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section == "General") section.clear();
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(std::string_view(line).substr(0, eq));
        for (char& c : key) {
            if (c == '\\') c = '/';
        }
        out[section.empty() ? key : section + "/" + key] = unescapeIniValue(std::string_view(line).substr(eq + 1));
        if (end == text.size()) break;
    }
    return out;
}

} // namespace bv::remote::qt
