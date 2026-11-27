#pragma once

#include "blackvolt/bv_api.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bv::runtime {

enum class Origin { Official, Community };

// One known build of an NVIDIA runtime DLL (see runtime/manifest.json).
struct KnownRuntime {
    bv_feature feature = BV_FEATURE_DLSS_SR;
    std::string file;     // e.g. "nvngx_dlss.dll"
    std::string version;  // e.g. "310.9.16.0"
    std::string sha256;   // lower-case hex
    uint64_t bytes = 0;
    Origin origin = Origin::Official;
    std::string label;    // human readable, e.g. "DLSS 310.9.16 (NVIDIA)"
    bool bundled = false; // shipped inside Release packages
    std::string slot;     // "" = runtime/, "legacy" = runtime/legacy/ (RTX 20/30)
};

class Manifest {
public:
    // Parses the manifest JSON. Returns nullopt and fills `error` on malformed input.
    static std::optional<Manifest> parse(std::string_view json, std::string* error = nullptr);
    static std::optional<Manifest> load(const std::string& path, std::string* error = nullptr);

    [[nodiscard]] const std::vector<KnownRuntime>& entries() const { return entries_; }
    [[nodiscard]] const KnownRuntime* findByHash(std::string_view sha256) const;
    [[nodiscard]] std::vector<const KnownRuntime*> forFeature(bv_feature f) const;

    static std::optional<bv_feature> featureFromKey(std::string_view key);
    static std::string_view featureKey(bv_feature f);
    // Canonical DLL name NGX loads for a feature.
    static std::string_view dllName(bv_feature f);

private:
    std::vector<KnownRuntime> entries_;
};

} // namespace bv::runtime
