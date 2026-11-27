#pragma once

#include "blackvolt/bv_api.h"
#include "core/Engine.h"

// The opaque handle behind bv_engine*, shared by every API translation unit.
struct bv_engine {
    bv_engine(std::unique_ptr<bv::gpu::D3D12Context> gpu, bv::runtime::RuntimePaths paths)
        : engine(std::move(gpu), std::move(paths)) {}
    bv::Engine engine;
};
