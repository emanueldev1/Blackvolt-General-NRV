#pragma once

#include "blackvolt/bv_api.h"

#include <cstdint>

namespace bv::ngx {

// NGX feature IDs. SR/RR/FG are named in the public DLSS SDK headers; VSR, TrueHDR and NR
// use IDs the public header lists as reserved (16, 14, 18), loaded from their own DLLs.
constexpr uint32_t featureId(bv_feature f) {
    switch (f) {
    case BV_FEATURE_DLSS_SR: return 1;
    case BV_FEATURE_DLSS_FG: return 11;
    case BV_FEATURE_DLSS_RR: return 13;
    case BV_FEATURE_RTX_TRUEHDR: return 14;
    case BV_FEATURE_RTX_VSR: return 16;
    case BV_FEATURE_DLSS_NR: return 18;
    default: return 0;
    }
}

// NVSDK_NGX_Feature_Support_Result bits -> Blackvolt runtime state.
constexpr bv_runtime_state stateFromSupportBits(uint32_t bits) {
    if (bits == 0) return BV_RUNTIME_READY;
    if (bits & 2u) return BV_RUNTIME_DRIVER_TOO_OLD;   // DriverVersionUnsupported
    if (bits & 4u) return BV_RUNTIME_UNSUPPORTED_GPU;  // AdapterUnsupported
    if (bits & 8u) return BV_RUNTIME_UNSUPPORTED_GPU;  // OS below minimum
    // CheckNotPresent / NotImplemented: the DLL has no requirement table; creation decides.
    return BV_RUNTIME_FOUND;
}

} // namespace bv::ngx
