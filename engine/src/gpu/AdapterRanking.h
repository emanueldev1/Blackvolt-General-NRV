#pragma once

#include "blackvolt/bv_api.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace bv::gpu {

inline constexpr uint32_t kVendorNvidia = 0x10DE;
inline constexpr uint32_t kVendorAmd = 0x1002;
inline constexpr uint32_t kVendorIntel = 0x8086;

bv_gpu_vendor vendorFromId(uint32_t vendorId);

// Capabilities that can be inferred from identity alone. Runtime probes
// (RTX Video SDK, NVENC session) refine these once the SDKs are wired in.
uint32_t inferCapabilities(uint32_t vendorId, std::string_view name);

// True for GeForce RTX 20 / 30 series (Turing / Ampere) by marketing name.
bool isTuringOrAmpereRtx(std::string_view name);

// Higher is better. RTX > other NVIDIA > other discrete > integrated; VRAM breaks ties.
int64_t score(const bv_adapter_info& info);

// Index into `adapters` of the best one, or -1 if empty.
int bestAdapter(std::span<const bv_adapter_info> adapters);

// NVIDIA formats the user-mode driver version 32.0.15.6636 as "566.36".
// Returns false when the version cannot be derived.
bool nvidiaDriverVersion(uint64_t umdVersion, char* out, std::size_t outSize);

} // namespace bv::gpu
