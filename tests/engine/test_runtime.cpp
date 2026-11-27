#include "check.h"
#include "runtime/Catalog.h"
#include "runtime/Manifest.h"
#include "runtime/Sha256.h"
#include "ngx/NgxFeatures.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace bv::runtime;

static std::span<const std::byte> bytesOf(const std::string& s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

TEST(sha256_known_vectors) {
    CHECK(Sha256::ofBytes(bytesOf("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::ofBytes(bytesOf("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::ofBytes(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1'000'000, 'a');
    CHECK(Sha256::ofBytes(bytesOf(million)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_streaming_equals_one_shot) {
    std::string data(10'000, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = char(i * 31 + 7);
    Sha256 s;
    for (std::size_t off = 0; off < data.size(); off += 97) {
        s.update(bytesOf(data.substr(off, 97)));
    }
    CHECK(Sha256::hex(s.finish()) == Sha256::ofBytes(bytesOf(data)));
}

TEST(repo_manifest_parses) {
    std::string error;
    const auto m = Manifest::load(std::string(BV_SOURCE_DIR) + "/runtime/manifest.json", &error);
    CHECK(m.has_value());
    if (!m) { std::printf("  %s\n", error.c_str()); return; }
    CHECK(m->forFeature(BV_FEATURE_DLSS_SR).size() == 1);
    // No DLSS NR runtime is shipped or recognised until NVIDIA publishes it officially.
    CHECK(m->forFeature(BV_FEATURE_DLSS_NR).empty());
    const auto* sr = m->findByHash("D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E");
    CHECK(sr && sr->bundled && sr->origin == Origin::Official);
    for (const auto& r : m->entries()) CHECK(r.origin == Origin::Official);
}

TEST(manifest_rejects_bad_input) {
    CHECK(!Manifest::parse("not json").has_value());
    CHECK(!Manifest::parse(R"({"schema":2,"runtimes":[]})").has_value());
    CHECK(!Manifest::parse(R"({"schema":1,"runtimes":[{"feature":"warp_drive","sha256":"00"}]})").has_value());
    CHECK(!Manifest::parse(R"({"schema":1,"runtimes":[{"feature":"dlss_sr","sha256":"abc"}]})").has_value());
}

TEST(feature_ids_match_ngx) {
    using bv::ngx::featureId;
    CHECK(featureId(BV_FEATURE_DLSS_SR) == 1);
    CHECK(featureId(BV_FEATURE_DLSS_FG) == 11);
    CHECK(featureId(BV_FEATURE_DLSS_RR) == 13);
    CHECK(featureId(BV_FEATURE_DLSS_NR) == 18);
    CHECK(bv::ngx::stateFromSupportBits(0) == BV_RUNTIME_READY);
    CHECK(bv::ngx::stateFromSupportBits(2) == BV_RUNTIME_DRIVER_TOO_OLD);
    CHECK(bv::ngx::stateFromSupportBits(4) == BV_RUNTIME_UNSUPPORTED_GPU);
}

namespace {
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("bv-test-" + std::to_string(std::rand()) + std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string manifestFor(const std::string& sha, std::size_t bytes) {
    return R"({"schema":1,"runtimes":[{"feature":"dlss_nr","file":"nvngx_dlssnr.dll","sha256":")" + sha +
           R"(","bytes":)" + std::to_string(bytes) + R"(,"origin":"community","label":"NR test build"}]})";
}
} // namespace

TEST(catalog_import_scan_and_locate) {
    TempDir tmp;
    const std::string dll = "MZ fake runtime payload";
    const auto sha = Sha256::ofBytes(bytesOf(dll));
    writeFile(tmp.path / "manifest.json", manifestFor(sha, dll.size()));

    // A fake "Veyra install" with the DLL nested a few folders deep and an unrelated DLL.
    writeFile(tmp.path / "veyra/runtime/experimental/nvngx_dlssnr.dll", dll);
    writeFile(tmp.path / "veyra/runtime/other.dll", "MZ other");

    Catalog catalog({tmp.path / "bundled", tmp.path / "user", tmp.path / "manifest.json"});
    CHECK(!catalog.locate(BV_FEATURE_DLSS_NR).has_value());

    const auto found = catalog.scan(tmp.path / "veyra");
    CHECK(found.size() == 1);
    if (found.empty()) return;
    CHECK(found[0].feature == BV_FEATURE_DLSS_NR);
    CHECK(found[0].id.origin == BV_ORIGIN_COMMUNITY);
    CHECK(found[0].id.label == "NR test build");

    Located imported;
    CHECK(catalog.importFile(found[0].path, BV_FEATURE_DLSS_NR, &imported) == Catalog::ImportError::None);
    CHECK(fs::exists(tmp.path / "user/nvngx_dlssnr.dll"));
    CHECK(imported.id.origin == BV_ORIGIN_COMMUNITY);

    const auto located = catalog.locate(BV_FEATURE_DLSS_NR);
    CHECK(located.has_value());
    CHECK(located && located->path == tmp.path / "user/nvngx_dlssnr.dll");
    CHECK(fs::exists(tmp.path / "user/hash-cache.json"));
}

TEST(catalog_rejects_wrong_files) {
    TempDir tmp;
    writeFile(tmp.path / "manifest.json", manifestFor(std::string(64, 'a'), 1));
    writeFile(tmp.path / "notes.txt", "hello");
    writeFile(tmp.path / "random.dll", "MZ random");
    Catalog catalog({{}, tmp.path / "user", tmp.path / "manifest.json"});
    CHECK(catalog.importFile(tmp.path / "missing.dll", BV_FEATURE_DLSS_NR, nullptr) == Catalog::ImportError::NotFound);
    CHECK(catalog.importFile(tmp.path / "notes.txt", BV_FEATURE_DLSS_NR, nullptr) == Catalog::ImportError::NotADll);
    CHECK(catalog.importFile(tmp.path / "random.dll", BV_FEATURE_DLSS_NR, nullptr) == Catalog::ImportError::WrongFeature);
}

TEST(user_import_overrides_bundled) {
    TempDir tmp;
    writeFile(tmp.path / "manifest.json", manifestFor(std::string(64, 'a'), 1));
    writeFile(tmp.path / "bundled/nvngx_dlss.dll", "MZ bundled");
    writeFile(tmp.path / "user/nvngx_dlss.dll", "MZ user");
    Catalog catalog({tmp.path / "bundled", tmp.path / "user", tmp.path / "manifest.json"});
    const auto located = catalog.locate(BV_FEATURE_DLSS_SR);
    CHECK(located && located->path == tmp.path / "user/nvngx_dlss.dll");
    CHECK(catalog.searchPaths().size() == 2);
}

TEST(legacy_slot_preferred_on_turing_ampere) {
    TempDir tmp;
    writeFile(tmp.path / "manifest.json", manifestFor(std::string(64, 'a'), 1));
    writeFile(tmp.path / "bundled/nvngx_dlssnr.dll", "MZ default");
    writeFile(tmp.path / "bundled/legacy/nvngx_dlssnr.dll", "MZ legacy");
    Catalog catalog({tmp.path / "bundled", tmp.path / "user", tmp.path / "manifest.json"});
    CHECK(catalog.locate(BV_FEATURE_DLSS_NR)->path == tmp.path / "bundled/nvngx_dlssnr.dll");
    catalog.setPreferLegacy(true);
    CHECK(catalog.locate(BV_FEATURE_DLSS_NR)->path == tmp.path / "bundled/legacy/nvngx_dlssnr.dll");
}
