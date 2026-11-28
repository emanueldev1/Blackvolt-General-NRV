#pragma once

#include <cstdint>

namespace bv::ngx {

// Which nvngx_dlssnr.dll build to load. The files live in <runtime>/nr/<folder>/.
enum class NrVariant : uint32_t {
    Auto = 0,        // compat if present, else NVIDIA original
    Compat = 1,      // community FP16 build (ShortFuse "SF"): RTX 20, 30, 40 and 50
    Original = 2,    // unmodified NVIDIA build: RTX 50 (Blackwell) only
};

constexpr const wchar_t* nrFolder(NrVariant v) { return v == NrVariant::Original ? L"original" : L"compat"; }
constexpr const char* nrVariantName(NrVariant v) {
    return v == NrVariant::Original ? "NVIDIA original (RTX 50)" : "Compatible SF (RTX 20-50)";
}

// The compat route optionally reports Blackwell to the NR module for an Ampere adapter
// (architecture 0x170 -> 0x1B0), so builds that keep Ampere-runnable kernels in the Blackwell
// slot select them. Only the selected adapter, only a successful query.
constexpr bool rewriteNrAmpereArchitecture(uint32_t& architecture, bool selected, int result) {
    if (result != 0 || !selected || (architecture & 0xFFFFFFF0u) != 0x170u) return false;
    architecture = 0x1B0u;
    return true;
}

// The ShortFuse "SF" family (310.8.2.x, the later 310.8.3.x) and the FP16 310.9.16.0 build pick
// their route from the real architecture: reporting Blackwell there would select kernels Ampere
// cannot run. Older or unknown builds keep the rewrite. Versions come from VS_FIXEDFILEINFO.
constexpr bool nrRuntimeNeedsAmpereRewrite(uint32_t fileVersionMs, uint32_t fileVersionLs) {
    const uint32_t major = fileVersionMs >> 16, minor = fileVersionMs & 0xFFFFu, build = fileVersionLs >> 16;
    const bool sfLegacy = major == 310u && minor == 8u && build >= 2u;
    const bool sf310916 = fileVersionMs == ((310u << 16) | 9u) && fileVersionLs == (16u << 16);
    return !(sfLegacy || sf310916);
}

// Create-time model choice. The 310.9.16 compat build embeds two models: preset 2 is the newer
// "B" (performance) model, 1 the older "A". The NVIDIA original takes 0.
constexpr int32_t nrRenderPreset(NrVariant v, bool performanceModel) {
    return v == NrVariant::Compat ? (performanceModel ? 2 : 1) : 0;
}

} // namespace bv::ngx
