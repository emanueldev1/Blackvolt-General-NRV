#include "Engine.h"

#include "gpu/AdapterRanking.h"
#include "runtime/Paths.h"

#include <algorithm>
#include <cstring>

namespace bv {

namespace {
template <std::size_t N>
void copyString(char (&dst)[N], const std::string& src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}
} // namespace

void fillStatus(bv_runtime_status& out, bv_feature feature, const runtime::Located* located, bv_runtime_state state) {
    out = bv_runtime_status{};
    out.size = sizeof(out);
    out.feature = feature;
    out.state = located ? state : BV_RUNTIME_MISSING;
    if (located) {
        out.origin = located->id.origin;
        copyString(out.label, located->id.label);
        copyString(out.path, runtime::toUtf8(located->path));
    }
}

Engine::Engine(std::unique_ptr<gpu::D3D12Context> gpu, runtime::RuntimePaths paths)
    : gpu_(std::move(gpu)), catalog_(std::move(paths)) {
    const bool nvidia = gpu_->adapter().vendor == BV_VENDOR_NVIDIA;
    catalog_.setPreferLegacy(nvidia && gpu::isTuringOrAmpereRtx(gpu_->adapter().name));
    if (nvidia && ngx::NgxRuntime::kCompiledIn) {
        auto dataPath = catalog_.paths().userDir.empty() ? std::filesystem::temp_directory_path() / "Blackvolt"
                                                         : catalog_.paths().userDir.parent_path() / "ngx";
        ngx_ = ngx::NgxRuntime::create(gpu_->device(), gpu_->dxgiAdapter(), catalog_.searchPaths(), dataPath);
    }
}

Engine::~Engine() {
    ngx_.reset(); // NGX must shut down while the device is still alive
}

bv_runtime_status Engine::runtimeStatus(bv_feature feature) {
    bv_runtime_status status{};
    const auto located = catalog_.locate(feature);
    bv_runtime_state state = BV_RUNTIME_MISSING;
    if (located) {
        if (!ngx_) state = BV_RUNTIME_NGX_UNAVAILABLE;
        // NR is not driven through the NGX core (see ngx/DlssNrSession.h): the core cannot
        // answer for it, creating the feature decides.
        else if (feature == BV_FEATURE_DLSS_NR) state = ngx_->initialized() ? BV_RUNTIME_FOUND : BV_RUNTIME_NGX_UNAVAILABLE;
        else state = ngx_->support(feature);
    }
    fillStatus(status, feature, located ? &*located : nullptr, state);
    return status;
}

} // namespace bv
