#include "Manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace bv::runtime {
namespace {

struct FeatureName { bv_feature feature; std::string_view key; std::string_view dll; };

constexpr FeatureName kFeatures[] = {
    {BV_FEATURE_DLSS_SR, "dlss_sr", "nvngx_dlss.dll"},
    {BV_FEATURE_DLSS_RR, "dlss_rr", "nvngx_dlssd.dll"},
    {BV_FEATURE_DLSS_FG, "dlss_fg", "nvngx_dlssg.dll"},
    {BV_FEATURE_DLSS_NR, "dlss_nr", "nvngx_dlssnr.dll"},
    {BV_FEATURE_RTX_VSR, "rtx_vsr", "nvngx_vsr.dll"},
    {BV_FEATURE_RTX_TRUEHDR, "rtx_truehdr", "nvngx_truehdr.dll"},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

std::optional<bv_feature> Manifest::featureFromKey(std::string_view key) {
    for (const auto& f : kFeatures) {
        if (f.key == key) return f.feature;
    }
    return std::nullopt;
}

std::string_view Manifest::featureKey(bv_feature f) {
    for (const auto& e : kFeatures) {
        if (e.feature == f) return e.key;
    }
    return "unknown";
}

std::string_view Manifest::dllName(bv_feature f) {
    for (const auto& e : kFeatures) {
        if (e.feature == f) return e.dll;
    }
    return {};
}

std::optional<Manifest> Manifest::parse(std::string_view json, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<Manifest> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    const auto doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded() || !doc.is_object()) return fail("manifest is not a JSON object");
    if (doc.value("schema", 0) != 1) return fail("unsupported manifest schema");
    const auto it = doc.find("runtimes");
    if (it == doc.end() || !it->is_array()) return fail("manifest has no runtimes array");

    Manifest m;
    for (const auto& r : *it) {
        if (!r.is_object()) return fail("runtime entry is not an object");
        const auto feature = featureFromKey(r.value("feature", ""));
        if (!feature) return fail("unknown feature: " + r.value("feature", ""));
        KnownRuntime k;
        k.feature = *feature;
        k.file = r.value("file", std::string(dllName(*feature)));
        k.version = r.value("version", "");
        k.sha256 = lower(r.value("sha256", ""));
        k.bytes = r.value("bytes", uint64_t{0});
        k.origin = r.value("origin", "official") == "community" ? Origin::Community : Origin::Official;
        k.label = r.value("label", k.file + " " + k.version);
        k.bundled = r.value("bundled", false);
        k.slot = r.value("slot", "");
        if (k.sha256.size() != 64) return fail("invalid sha256 for " + k.label);
        m.entries_.push_back(std::move(k));
    }
    return m;
}

std::optional<Manifest> Manifest::load(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), error);
}

const KnownRuntime* Manifest::findByHash(std::string_view sha256) const {
    const auto key = lower(std::string(sha256));
    for (const auto& e : entries_) {
        if (e.sha256 == key) return &e;
    }
    return nullptr;
}

std::vector<const KnownRuntime*> Manifest::forFeature(bv_feature f) const {
    std::vector<const KnownRuntime*> out;
    for (const auto& e : entries_) {
        if (e.feature == f) out.push_back(&e);
    }
    return out;
}

} // namespace bv::runtime
