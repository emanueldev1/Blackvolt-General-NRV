#include "check.h"
#include "gpu/AdapterRanking.h"

#include <cstring>
#include <string_view>
#include <vector>

using namespace bv::gpu;

static bv_adapter_info make(uint32_t vendor, const char* name, uint64_t vramMiB) {
    bv_adapter_info a{};
    a.size = sizeof(a);
    a.vendor_id = vendor;
    a.vendor = vendorFromId(vendor);
    a.dedicated_vram_bytes = vramMiB << 20;
    std::strncpy(a.name, name, sizeof(a.name) - 1);
    a.capabilities = inferCapabilities(vendor, name);
    return a;
}

TEST(rtx_gets_video_sr_and_truehdr) {
    const auto caps = inferCapabilities(kVendorNvidia, "NVIDIA GeForce RTX 4070");
    CHECK(caps & BV_CAP_RTX_VIDEO_SR);
    CHECK(caps & BV_CAP_RTX_TRUE_HDR);
    CHECK(caps & BV_CAP_OPTICAL_FLOW);
    CHECK(caps & BV_CAP_NVENC);
}

TEST(gtx10_has_no_rtx_features) {
    const auto caps = inferCapabilities(kVendorNvidia, "NVIDIA GeForce GTX 1080");
    CHECK(!(caps & BV_CAP_RTX_VIDEO_SR));
    CHECK(!(caps & BV_CAP_OPTICAL_FLOW));
    CHECK(caps & BV_CAP_NVENC);
}

TEST(amd_has_only_d3d12) {
    CHECK(inferCapabilities(kVendorAmd, "AMD Radeon RX 7900 XTX") == BV_CAP_D3D12);
}

TEST(rtx_beats_bigger_amd_and_igpu) {
    std::vector<bv_adapter_info> list{
        make(kVendorIntel, "Intel(R) UHD Graphics 770", 128),
        make(kVendorAmd, "AMD Radeon RX 7900 XTX", 24576),
        make(kVendorNvidia, "NVIDIA GeForce RTX 3060", 12288),
    };
    CHECK(bestAdapter(list) == 2);
}

TEST(empty_list_has_no_best) {
    std::vector<bv_adapter_info> list;
    CHECK(bestAdapter(list) == -1);
}

TEST(nvidia_driver_version_format) {
    // 32.0.15.6636 -> 566.36
    const uint64_t umd = (32ull << 48) | (0ull << 32) | (15ull << 16) | 6636ull;
    char buf[32]{};
    CHECK(nvidiaDriverVersion(umd, buf, sizeof(buf)));
    CHECK(std::string_view(buf) == "566.36");
}

TEST(legacy_rtx_detection) {
    CHECK(isTuringOrAmpereRtx("NVIDIA GeForce RTX 2060 SUPER"));
    CHECK(isTuringOrAmpereRtx("NVIDIA GeForce RTX 3080 Ti"));
    CHECK(isTuringOrAmpereRtx("NVIDIA RTX A4000"));
    CHECK(!isTuringOrAmpereRtx("NVIDIA GeForce RTX 4070"));
    CHECK(!isTuringOrAmpereRtx("NVIDIA GeForce RTX 5090"));
    CHECK(!isTuringOrAmpereRtx("NVIDIA GeForce GTX 1080"));
}
