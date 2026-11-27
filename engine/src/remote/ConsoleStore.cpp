#include "ConsoleStore.h"

#include "ChiakiImport.h"
#include "Protect.h"
#include "VeyraImport.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <iterator>

namespace bv::remote {
namespace {

constexpr char kMagic[4] = {'B', 'V', 'C', '1'};

template <std::size_t N> std::string hex(const std::array<uint8_t, N>& a) { return hexEncode(a.data(), N); }

template <std::size_t N> bool unhex(const nlohmann::json& j, const char* key, std::array<uint8_t, N>& out) {
    if (!j.contains(key) || !j[key].is_string()) return false;
    const auto s = j[key].get<std::string>();
    if (s.size() != N * 2) return false;
    for (std::size_t i = 0; i < N; ++i) {
        const auto byte = s.substr(i * 2, 2);
        char* end = nullptr;
        const auto v = std::strtoul(byte.c_str(), &end, 16);
        if (end != byte.c_str() + 2) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

} // namespace

std::vector<uint8_t> ConsoleStore::serialize(const std::vector<ConsoleProfile>& consoles) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& c : consoles) {
        nlohmann::json j{
            {"nickname", c.nickname}, {"host", c.host}, {"kind", c.kind == ConsoleKind::PS5 ? "ps5" : "ps4"},
            {"target", c.target}, {"regist_key", hex(c.registKey)}, {"rp_key", hex(c.rpKey)},
            {"rp_key_type", c.rpKeyType}, {"source", c.source},
        };
        if (c.hasMac) j["mac"] = c.macHex();
        if (c.hasAccountId) j["psn_account_id"] = hex(c.psnAccountId);
        if (c.prefs) {
            j["prefs"] = {{"width", c.prefs->width}, {"height", c.prefs->height}, {"fps", c.prefs->fps},
                          {"bitrate_kbps", c.prefs->bitrateKbps}, {"codec", c.prefs->codec},
                          {"view_only", c.prefs->viewOnly}};
        }
        list.push_back(std::move(j));
    }
    const std::string text = nlohmann::json{{"schema", 1}, {"consoles", list}}.dump();
    return {text.begin(), text.end()};
}

std::optional<std::vector<ConsoleProfile>> ConsoleStore::deserialize(std::span<const uint8_t> data) {
    const auto doc = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("schema", 0) != 1 || !doc.contains("consoles")) {
        return std::nullopt;
    }
    std::vector<ConsoleProfile> out;
    for (const auto& j : doc["consoles"]) {
        if (!j.is_object()) continue;
        ConsoleProfile c;
        if (!unhex(j, "regist_key", c.registKey) || !unhex(j, "rp_key", c.rpKey)) continue;
        c.nickname = j.value("nickname", "");
        c.host = j.value("host", "");
        c.kind = j.value("kind", "ps5") == "ps4" ? ConsoleKind::PS4 : ConsoleKind::PS5;
        c.target = j.value("target", 1000100u);
        c.rpKeyType = j.value("rp_key_type", 0u);
        c.source = j.value("source", "blackvolt");
        if (j.contains("mac") && j["mac"].is_string()) {
            if (auto mac = parseMacHex(j["mac"].get<std::string>())) {
                c.mac = *mac;
                c.hasMac = true;
            }
        }
        c.hasAccountId = unhex(j, "psn_account_id", c.psnAccountId);
        if (j.contains("prefs") && j["prefs"].is_object()) {
            const auto& p = j["prefs"];
            c.prefs = ConsoleProfile::StreamPrefs{p.value("width", 0u),        p.value("height", 0u),
                                                  p.value("fps", 0u),          p.value("bitrate_kbps", 0u),
                                                  p.value("codec", 0u),        p.value("view_only", false)};
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool ConsoleStore::load() {
    consoles_.clear();
    std::ifstream in(file_, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < sizeof(kMagic) || std::memcmp(raw.data(), kMagic, sizeof(kMagic)) != 0) return false;
    auto plain = unprotectForUser(std::span(raw).subspan(sizeof(kMagic)));
    if (!plain) return false;
    auto parsed = deserialize(*plain);
    secureWipe(plain->data(), plain->size());
    if (!parsed) return false;
    consoles_ = std::move(*parsed);
    return true;
}

bool ConsoleStore::save() const {
    auto plain = serialize(consoles_);
    auto sealed = protectForUser(plain);
    secureWipe(plain.data(), plain.size());
    if (!sealed) return false;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto part = file_;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(kMagic, sizeof(kMagic));
        out.write(reinterpret_cast<const char*>(sealed->data()), static_cast<std::streamsize>(sealed->size()));
        if (!out) return false;
    }
    std::filesystem::rename(part, file_, ec);
    return !ec;
}

std::size_t ConsoleStore::importFromInstalledApps() {
    std::size_t added = import(chiaki::fromRegistry());
    added += import(veyra::fromDirectory(veyra::defaultDirectory()));
    return added;
}

bool ConsoleStore::remove(std::size_t index) {
    if (index >= consoles_.size()) return false;
    auto& c = consoles_[index];
    secureWipe(c.registKey.data(), c.registKey.size());
    secureWipe(c.rpKey.data(), c.rpKey.size());
    consoles_.erase(consoles_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace bv::remote
