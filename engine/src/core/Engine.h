#pragma once

#include "enhance/NeuralRendering.h"
#include "gpu/D3D12Context.h"
#include "ngx/NgxRuntime.h"
#include "runtime/Catalog.h"
#include "telemetry/Telemetry.h"

#include <memory>

namespace bv {

// One engine = one GPU device, its NVIDIA runtimes and its telemetry. Sources, enhancement
// stages and presenters attach to it as the pipeline grows (see docs/ARCHITECTURE.md).
class Engine {
public:
    Engine(std::unique_ptr<gpu::D3D12Context> gpu, runtime::RuntimePaths paths);
    ~Engine();

    [[nodiscard]] gpu::D3D12Context& gpu() { return *gpu_; }
    [[nodiscard]] telemetry::Telemetry& telemetry() { return telemetry_; }
    [[nodiscard]] runtime::Catalog& catalog() { return catalog_; }
    [[nodiscard]] ngx::NgxRuntime* ngx() { return ngx_.get(); }

    bv_runtime_status runtimeStatus(bv_feature feature);
    // Where each DLSS NR build lives, for the presenters (the catalog outlives them).
    [[nodiscard]] enhance::NrLocator nrLocator() {
        return [this](ngx::NrVariant v) { return catalog_.nrFolder(v); };
    }

private:
    std::unique_ptr<gpu::D3D12Context> gpu_;
    runtime::Catalog catalog_;
    std::unique_ptr<ngx::NgxRuntime> ngx_;
    telemetry::Telemetry telemetry_;
};

// Fills a status struct from a located runtime (shared by the engine and the import API).
void fillStatus(bv_runtime_status& out, bv_feature feature, const runtime::Located* located, bv_runtime_state state);

} // namespace bv
