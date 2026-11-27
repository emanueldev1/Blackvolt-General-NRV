#include "AdapterRanking.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace bv::gpu {
namespace {

bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

bv_gpu_vendor vendorFromId(uint32_t vendorId) {
    switch (vendorId) {
    case kVendorNvidia: return BV_VENDOR_NVIDIA;
    case kVendorAmd: return BV_VENDOR_AMD;
    case kVendorIntel: return BV_VENDOR_INTEL;
    default: return BV_VENDOR_UNKNOWN;
    }
}

uint32_t inferCapabilities(uint32_t vendorId, std::string_view name) {
    uint32_t caps = BV_CAP_D3D12;
    if (vendorId != kVendorNvidia) return caps;

    caps |= BV_CAP_NVENC | BV_CAP_NVDEC;
    const bool rtx = containsNoCase(name, "RTX");
    const bool gtx16 = containsNoCase(name, "GTX 16");
    if (rtx) caps |= BV_CAP_RTX_VIDEO_SR | BV_CAP_RTX_TRUE_HDR;
    if (rtx || gtx16) caps |= BV_CAP_OPTICAL_FLOW; // Turing and newer
    return caps;
}

int64_t score(const bv_adapter_info& info) {
    int64_t s = 0;
    if (info.capabilities & BV_CAP_RTX_VIDEO_SR) s += 4'000'000;
    if (info.vendor == BV_VENDOR_NVIDIA) s += 2'000'000;
    // Discrete GPUs have dedicated memory; integrated ones report little or none.
    if (info.dedicated_vram_bytes >= (512ull << 20)) s += 1'000'000;
    s += static_cast<int64_t>(info.dedicated_vram_bytes >> 20); // MiB as tie-breaker
    return s;
}

int bestAdapter(std::span<const bv_adapter_info> adapters) {
    int best = -1;
    int64_t bestScore = -1;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const auto sc = score(adapters[i]);
        if (sc > bestScore) {
            bestScore = sc;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool nvidiaDriverVersion(uint64_t umdVersion, char* out, std::size_t outSize) {
    if (!out || outSize < 8) return false;
    const auto build = static_cast<uint32_t>(umdVersion & 0xFFFF);          // 6636
    const auto minor = static_cast<uint32_t>((umdVersion >> 16) & 0xFFFF); // 15
    const uint32_t combined = (minor % 10) * 10000 + build;                 // 56636
    if (combined < 10000) return false;
    std::snprintf(out, outSize, "%u.%02u", combined / 100, combined % 100);
    return true;
}

} // namespace bv::gpu
