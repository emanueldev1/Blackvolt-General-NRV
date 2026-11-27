#pragma once

#include "gpu/D3D12Context.h"
#include "telemetry/Telemetry.h"

#include <memory>

namespace bv {

// One engine = one GPU device + its telemetry. Sources, enhancement stages and
// presenters attach to it as the pipeline grows (see docs/ARCHITECTURE.md).
class Engine {
public:
    explicit Engine(std::unique_ptr<gpu::D3D12Context> gpu) : gpu_(std::move(gpu)) {}

    [[nodiscard]] gpu::D3D12Context& gpu() { return *gpu_; }
    [[nodiscard]] telemetry::Telemetry& telemetry() { return telemetry_; }

private:
    std::unique_ptr<gpu::D3D12Context> gpu_;
    telemetry::Telemetry telemetry_;
};

} // namespace bv
