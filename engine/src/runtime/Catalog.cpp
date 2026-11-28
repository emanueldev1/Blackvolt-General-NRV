#include "Catalog.h"

#include "Paths.h"
#include "Sha256.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace bv::runtime {
namespace {

std::mutex g_pathsMutex;
RuntimePaths g_paths;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool looksLikePe(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<char, 2> magic{};
    return in.read(magic.data(), 2) && magic[0] == 'M' && magic[1] == 'Z';
}

std::optional<bv_feature> featureForFileName(const fs::path& file) {
    const auto name = lower(toUtf8(file.filename()));
    for (int f = 0; f < BV_FEATURE_COUNT; ++f) {
        if (name == Manifest::dllName(static_cast<bv_feature>(f))) return static_cast<bv_feature>(f);
    }
    return std::nullopt;
}

int64_t mtimeOf(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

} // namespace

void setGlobalPaths(RuntimePaths paths) {
    std::lock_guard lock(g_pathsMutex);
    g_paths = std::move(paths);
}

RuntimePaths globalPaths() {
    std::lock_guard lock(g_pathsMutex);
    return g_paths;
}

Catalog::Catalog(RuntimePaths paths) : paths_(std::move(paths)) {
    if (!paths_.manifest.empty()) {
        std::string error;
        manifest_ = Manifest::load(toUtf8(paths_.manifest), &error);
        if (!manifest_) log::warn("runtime manifest not loaded: {}", error);
    }
}

std::vector<fs::path> Catalog::searchPaths() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!paths_.userDir.empty() && fs::is_directory(paths_.userDir, ec)) out.push_back(paths_.userDir);
    if (preferLegacy_ && !paths_.bundledDir.empty() && fs::is_directory(paths_.bundledDir / "legacy", ec)) {
        out.push_back(paths_.bundledDir / "legacy");
    }
    if (!paths_.bundledDir.empty() && fs::is_directory(paths_.bundledDir, ec)) out.push_back(paths_.bundledDir);
    return out;
}

std::optional<Located> Catalog::locate(bv_feature feature) {
    if (feature == BV_FEATURE_DLSS_NR) {
        if (auto nr = locateNr(ngx::NrVariant::Compat)) return nr;
        if (auto nr = locateNr(ngx::NrVariant::Original)) return nr;
    }
    const auto dll = fromUtf8(std::string(Manifest::dllName(feature)));
    for (const auto& dir : searchPaths()) {
        std::error_code ec;
        const auto candidate = dir / dll;
        if (fs::is_regular_file(candidate, ec)) return Located{candidate, identify(candidate, feature)};
    }
    return std::nullopt;
}

Identified Catalog::identify(const fs::path& file, bv_feature feature) {
    Identified id;
    id.label = toUtf8(file.filename());
    if (!manifest_) return id;
    const auto sha = hashOf(file);
    if (!sha) return id;
    if (const auto* known = manifest_->findByHash(*sha); known && known->feature == feature) {
        id.known = known;
        id.label = known->label;
        id.origin = known->origin == Origin::Community ? BV_ORIGIN_COMMUNITY : BV_ORIGIN_OFFICIAL;
    }
    return id;
}

Catalog::ImportError Catalog::importFile(const fs::path& source, bv_feature feature, Located* result) {
    std::error_code ec;
    if (paths_.userDir.empty()) return ImportError::NoUserDir;
    if (!fs::is_regular_file(source, ec)) return ImportError::NotFound;
    if (!looksLikePe(source)) return ImportError::NotADll;

    // Accept the file if its name is the feature's DLL name, or if the manifest knows its hash.
    const auto byName = featureForFileName(source);
    const auto id = identify(source, feature);
    if (byName != feature && id.known == nullptr) return ImportError::WrongFeature;

    fs::create_directories(paths_.userDir, ec);
    const auto target = paths_.userDir / fromUtf8(std::string(Manifest::dllName(feature)));
    const auto tmp = fs::path(target).concat(L".part");
    fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return ImportError::CopyFailed;
    fs::rename(tmp, target, ec); // atomic replace: a half-copied DLL is never picked up
    if (ec) {
        fs::remove(tmp, ec);
        return ImportError::CopyFailed;
    }
    log::info("imported {} -> {}", toUtf8(source), toUtf8(target));
    if (result) *result = Located{target, identify(target, feature)};
    return ImportError::None;
}

std::vector<Candidate> Catalog::scan(const fs::path& folder) {
    std::vector<Candidate> out;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return out;
    auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 6) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto feature = featureForFileName(it->path());
        if (!feature) continue;
        Candidate c{*feature, it->path(), identify(it->path(), *feature)};
        if (*feature == BV_FEATURE_DLSS_NR) c.nrVariant = nrVariantOf(c.path, c.id);
        out.push_back(std::move(c));
    }
    // Known builds first, then by feature.
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.feature != b.feature) return a.feature < b.feature;
        return (a.id.known != nullptr) > (b.id.known != nullptr);
    });
    return out;
}

fs::path Catalog::nrFolder(ngx::NrVariant variant) const {
    if (variant == ngx::NrVariant::Auto) return {};
    std::error_code ec;
    for (const auto& root : {paths_.userDir, paths_.bundledDir}) {
        if (root.empty()) continue;
        const auto folder = root / L"nr" / ngx::nrFolder(variant);
        if (fs::is_regular_file(folder / L"nvngx_dlssnr.dll", ec)) return folder;
    }
    return {};
}

std::optional<Located> Catalog::locateNr(ngx::NrVariant variant) {
    const auto folder = nrFolder(variant);
    if (folder.empty()) return std::nullopt;
    const auto file = folder / L"nvngx_dlssnr.dll";
    return Located{file, identify(file, BV_FEATURE_DLSS_NR)};
}

Catalog::ImportError Catalog::importNr(const fs::path& source, ngx::NrVariant variant, Located* result) {
    std::error_code ec;
    if (paths_.userDir.empty()) return ImportError::NoUserDir;
    if (variant == ngx::NrVariant::Auto) return ImportError::WrongFeature;
    if (!fs::is_regular_file(source, ec)) return ImportError::NotFound;
    if (!looksLikePe(source)) return ImportError::NotADll;
    const auto byName = featureForFileName(source);
    const auto id = identify(source, BV_FEATURE_DLSS_NR);
    if (byName != BV_FEATURE_DLSS_NR && id.known == nullptr) return ImportError::WrongFeature;

    const auto folder = paths_.userDir / L"nr" / ngx::nrFolder(variant);
    fs::create_directories(folder, ec);
    const auto target = folder / L"nvngx_dlssnr.dll";
    if (fs::exists(target, ec) && fs::equivalent(source, target, ec)) {
        if (result) *result = Located{target, id};
        return ImportError::None;
    }
    const auto tmp = fs::path(target).concat(L".part");
    fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return ImportError::CopyFailed;
    fs::rename(tmp, target, ec); // a half-copied DLL is never picked up
    if (ec) {
        fs::remove(tmp, ec);
        return ImportError::CopyFailed; // usually: the DLL is loaded by a running session
    }
    log::info("imported DLSS NR ({}) {} -> {}", ngx::nrVariantName(variant), toUtf8(source), toUtf8(target));
    if (result) *result = Located{target, identify(target, BV_FEATURE_DLSS_NR)};
    return ImportError::None;
}

bool Catalog::removeNr(ngx::NrVariant variant) {
    if (paths_.userDir.empty() || variant == ngx::NrVariant::Auto) return false;
    std::error_code ec;
    const auto file = paths_.userDir / L"nr" / ngx::nrFolder(variant) / L"nvngx_dlssnr.dll";
    return fs::remove(file, ec) && !ec;
}

ngx::NrVariant Catalog::nrVariantOf(const fs::path& file, const Identified& id) {
    for (const auto& part : file.parent_path()) {
        const auto name = lower(toUtf8(part));
        if (name == "nr-ampere" || name == "nr-community" || name == "compat") return ngx::NrVariant::Compat;
        if (name == "nr-original" || name == "original") return ngx::NrVariant::Original;
    }
    if (id.known) {
        if (id.known->slot == "compat") return ngx::NrVariant::Compat;
        if (id.known->slot == "original") return ngx::NrVariant::Original;
    }
    return ngx::NrVariant::Auto;
}

std::string Catalog::fileVersion(const fs::path& file) {
#if defined(_WIN32)
    // As a data file: nothing runs, only the resource section is mapped.
    HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return {};
    std::string out;
    if (HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16))) {
        const auto size = SizeofResource(module, info);
        const auto* bytes = static_cast<const uint8_t*>(LockResource(LoadResource(module, info)));
        for (DWORD at = 0; bytes && at + 52 <= size; at += 4) {
            uint32_t signature = 0, ms = 0, ls = 0;
            std::memcpy(&signature, bytes + at, 4);
            if (signature != 0xFEEF04BDu) continue;
            std::memcpy(&ms, bytes + at + 8, 4);
            std::memcpy(&ls, bytes + at + 12, 4);
            out = std::to_string(ms >> 16) + "." + std::to_string(ms & 0xFFFFu) + "." + std::to_string(ls >> 16) + "." +
                  std::to_string(ls & 0xFFFFu);
            break;
        }
    }
    FreeLibrary(module);
    return out;
#else
    (void)file;
    return {};
#endif
}

std::optional<std::string> Catalog::hashOf(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    const auto mtime = mtimeOf(file);
    const auto key = toUtf8(fs::absolute(file, ec));

    {
        std::lock_guard lock(mutex_);
        if (!cacheLoaded_) loadCache();
        if (auto it = cache_.find(key); it != cache_.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.sha256;
        }
    }
    // Skip hashing when no manifest entry has this size: it cannot be a known build.
    if (manifest_) {
        const bool sizeKnown = std::any_of(manifest_->entries().begin(), manifest_->entries().end(),
                                           [&](const KnownRuntime& k) { return k.bytes == 0 || k.bytes == size; });
        if (!sizeKnown) return std::string{};
    }
    auto sha = Sha256::ofFile(file);
    if (!sha) return std::nullopt;
    std::lock_guard lock(mutex_);
    cache_[key] = CacheEntry{size, mtime, *sha};
    saveCache();
    return sha;
}

void Catalog::loadCache() {
    cacheLoaded_ = true;
    if (paths_.userDir.empty()) return;
    std::ifstream in(paths_.userDir / "hash-cache.json", std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const auto doc = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!doc.is_object()) return;
    for (const auto& [path, v] : doc.items()) {
        if (!v.is_object()) continue;
        cache_[path] = CacheEntry{v.value("size", uint64_t{0}), v.value("mtime", int64_t{0}), v.value("sha256", "")};
    }
}

void Catalog::saveCache() {
    if (paths_.userDir.empty()) return;
    std::error_code ec;
    fs::create_directories(paths_.userDir, ec);
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [path, e] : cache_) doc[path] = {{"size", e.size}, {"mtime", e.mtime}, {"sha256", e.sha256}};
    std::ofstream out(paths_.userDir / "hash-cache.json", std::ios::binary | std::ios::trunc);
    out << doc.dump(2);
}

} // namespace bv::runtime
