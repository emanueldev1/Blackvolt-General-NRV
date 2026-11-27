#include "VideoSuperResolution.h"

#include "NgxRuntime.h"
#include "core/Log.h"

#if defined(BV_HAS_NGX)
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "NgxGuard.h"
#endif

namespace bv::ngx {

#if defined(BV_HAS_NGX)

struct VideoSuperResolution::Impl {
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;
    bool reportedFailure = false;

    ~Impl() {
        if (handle) guard::release(handle);
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
    }
};

std::unique_ptr<VideoSuperResolution> VideoSuperResolution::create(NgxRuntime* runtime, ID3D12GraphicsCommandList* list,
                                                                   uint32_t inWidth, uint32_t inHeight, uint32_t outWidth,
                                                                   uint32_t outHeight) {
    if (!runtime || !runtime->initialized() || !list) return nullptr;
    auto impl = std::make_unique<Impl>();
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&impl->params))) return nullptr;
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_Width, inWidth);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_Height, inHeight);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_OutWidth, outWidth);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_OutHeight, outHeight);
    // RTX Video SR is NGX feature 16 (reserved id in the public headers).
    uint32_t seh = 0;
    const auto r = guard::create(list, NVSDK_NGX_Feature_Reserved16, impl->params, &impl->handle, seh);
    if (NVSDK_NGX_FAILED(r) || !impl->handle) {
        log::warn("vsr: create failed (0x{:08X}, seh 0x{:08X}); driver or nvngx_vsr.dll missing?",
                  static_cast<uint32_t>(r), seh);
        impl->handle = nullptr;
        return nullptr;
    }
    log::info("vsr: RTX Video Super Resolution {}x{} -> {}x{}", inWidth, inHeight, outWidth, outHeight);
    return std::unique_ptr<VideoSuperResolution>(new VideoSuperResolution(std::move(impl)));
}

bool VideoSuperResolution::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, uint32_t inWidth,
                                    uint32_t inHeight, ID3D12Resource* output, uint32_t outWidth, uint32_t outHeight,
                                    uint32_t quality) {
    auto* p = impl_->params;
    NVSDK_NGX_Parameter_SetD3d12Resource(p, "Input1", input);
    NVSDK_NGX_Parameter_SetD3d12Resource(p, "Output", output);
    NVSDK_NGX_Parameter_SetUI(p, "Rect.X", 0);
    NVSDK_NGX_Parameter_SetUI(p, "Rect.Y", 0);
    NVSDK_NGX_Parameter_SetUI(p, "Rect.W", inWidth);
    NVSDK_NGX_Parameter_SetUI(p, "Rect.H", inHeight);
    NVSDK_NGX_Parameter_SetUI(p, "OutRect.X", 0);
    NVSDK_NGX_Parameter_SetUI(p, "OutRect.Y", 0);
    NVSDK_NGX_Parameter_SetUI(p, "OutRect.W", outWidth);
    NVSDK_NGX_Parameter_SetUI(p, "OutRect.H", outHeight);
    NVSDK_NGX_Parameter_SetUI(p, "VSR.QualityLevel", quality < 1 ? 1 : quality > 4 ? 4 : quality);
    uint32_t seh = 0;
    const auto r = guard::evaluate(list, impl_->handle, p, seh);
    if (NVSDK_NGX_FAILED(r)) {
        if (!impl_->reportedFailure)
            log::error("vsr: evaluate failed 0x{:08X} (seh 0x{:08X})", static_cast<uint32_t>(r), seh);
        impl_->reportedFailure = true;
        return false;
    }
    return true;
}

#else

struct VideoSuperResolution::Impl {};

std::unique_ptr<VideoSuperResolution> VideoSuperResolution::create(NgxRuntime*, ID3D12GraphicsCommandList*, uint32_t,
                                                                   uint32_t, uint32_t, uint32_t) {
    return nullptr;
}

bool VideoSuperResolution::evaluate(ID3D12GraphicsCommandList*, ID3D12Resource*, uint32_t, uint32_t, ID3D12Resource*,
                                    uint32_t, uint32_t, uint32_t) {
    return false;
}

#endif

VideoSuperResolution::VideoSuperResolution(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoSuperResolution::~VideoSuperResolution() = default;

} // namespace bv::ngx
