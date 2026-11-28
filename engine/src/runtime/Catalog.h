#pragma once

#include "Manifest.h"
#include "ngx/NrArchitecturePolicy.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bv::runtime {

struct RuntimePaths {
    std::filesystem::path bundledDir; // read-only, shipped with the app
    std::filesystem::path userDir;    // writable, imported runtimes
    std::filesystem::path manifest;
};

struct Identified {
    bv_runtime_origin origin = BV_ORIGIN_UNKNOWN;
    std::string label;
    const KnownRuntime* known = nullptr;
};

struct Located {
    std::filesystem::path path;
    Identified id;
};

struct Candidate {
    bv_feature feature;
    std::filesystem::path path;
    Identified id;
    ngx::NrVariant nrVariant = ngx::NrVariant::Auto; // DLSS NR only; Auto = unknown
};

// Finds, identifies and imports NVIDIA runtime DLLs.
// Hashing a 270 MB DLL takes ~0.5 s, so digests are cached by (path, size, mtime)
// in user_dir/hash-cache.json and only recomputed when the file changes.
class Catalog {
public:
    explicit Catalog(RuntimePaths paths);

    // RTX 20/30 (Turing/Ampere) prefer runtime/legacy/ builds compiled for their architecture.
    void setPreferLegacy(bool prefer) { preferLegacy_ = prefer; }

    [[nodiscard]] const RuntimePaths& paths() const { return paths_; }
    [[nodiscard]] const Manifest* manifest() const { return manifest_ ? &*manifest_ : nullptr; }

    // Folders NGX should search, highest priority first (imports override bundled files).
    [[nodiscard]] std::vector<std::filesystem::path> searchPaths() const;

    [[nodiscard]] std::optional<Located> locate(bv_feature feature);
    Identified identify(const std::filesystem::path& file, bv_feature feature);

    enum class ImportError { None, NotFound, NotADll, WrongFeature, CopyFailed, NoUserDir };
    ImportError importFile(const std::filesystem::path& source, bv_feature feature, Located* result);

    // Recursive search (depth <= 6) for nvngx_*.dll files that map to a known feature.
    std::vector<Candidate> scan(const std::filesystem::path& folder);

    // DLSS NR builds, one folder per variant: <user or bundled dir>/nr/<compat|original>/.
    // Returns the folder holding nvngx_dlssnr.dll, empty when that variant is not installed.
    [[nodiscard]] std::filesystem::path nrFolder(ngx::NrVariant variant) const;
    std::optional<Located> locateNr(ngx::NrVariant variant);
    ImportError importNr(const std::filesystem::path& source, ngx::NrVariant variant, Located* result);
    bool removeNr(ngx::NrVariant variant);
    // Which slot a nvngx_dlssnr.dll belongs to: from its folder (Veyra's nr-ampere / nr-original
    // layout or ours), else from the manifest. Auto when it cannot be told.
    static ngx::NrVariant nrVariantOf(const std::filesystem::path& file, const Identified& id);
    // VS_FIXEDFILEINFO version of a DLL ("310.9.16.0"), empty when unreadable.
    static std::string fileVersion(const std::filesystem::path& file);

private:
    std::optional<std::string> hashOf(const std::filesystem::path& file);
    void loadCache();
    void saveCache();

    struct CacheEntry { uint64_t size; int64_t mtime; std::string sha256; };

    RuntimePaths paths_;
    std::optional<Manifest> manifest_;
    std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
    bool cacheLoaded_ = false;
    bool preferLegacy_ = false;
};

// Process-wide runtime paths set through bv_set_runtime_paths().
void setGlobalPaths(RuntimePaths paths);
RuntimePaths globalPaths();

} // namespace bv::runtime
