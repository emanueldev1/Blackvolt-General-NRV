#pragma once

#include "blackvolt/bv_api.h"

#include <vector>

namespace bv::gpu {

// Hardware adapters that can create a D3D12 device (software/WARP adapters excluded).
// Returns an empty list on platforms without D3D12.
std::vector<bv_adapter_info> enumerateAdapters();

} // namespace bv::gpu
