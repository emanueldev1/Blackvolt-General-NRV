#include "TrueHdr.h"

#include "NgxRuntime.h"
#include "core/Log.h"

#include <algorithm>

#if defined(BV_HAS_NGX)
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "NgxGuard.h"
#endif

namespace bv::ngx {

TrueHdrParams TrueHdr::clamp(TrueHdrParams p) {
    p.contrast = std::min<uint32_t>(p.contrast, 200);
    p.saturation = std::min<uint32_t>(p.saturation, 200);
    p.middleGray = std::clamp<uint32_t>(p.middleGray, 10, 100);
    p.peakNits = std::clamp<uint32_t>(p.peakNits, 400, 2000);
    return p;
}

#if defined(BV_HAS_NGX)

struct TrueHdr::Impl {
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;
    bool reportedFailure = false;

    ~Impl() {
        if (handle) guard::release(handle);
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
    }
};

std::unique_ptr<TrueHdr> TrueHdr::create(NgxRuntime* runtime, ID3D12GraphicsCommandList* list) {
    if (!runtime || !runtime->initialized() || !list) return nullptr;
    auto impl = std::make_unique<Impl>();
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&impl->params))) return nullptr;
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(impl->params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    uint32_t seh = 0;
    // RTX Video TrueHDR is NGX feature 14 (reserved id in the public headers).
    const auto r = guard::create(list, NVSDK_NGX_Feature_Reserved14, impl->params, &impl->handle, seh);
    if (NVSDK_NGX_FAILED(r) || !impl->handle) {
        log::warn("truehdr: create failed (0x{:08X}, seh 0x{:08X}); driver or nvngx_truehdr.dll missing?",
                  static_cast<uint32_t>(r), seh);
        impl->handle = nullptr;
        return nullptr;
    }
    log::info("truehdr: RTX Video TrueHDR ready");
    return std::unique_ptr<TrueHdr>(new TrueHdr(std::move(impl)));
}

bool TrueHdr::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, uint32_t width,
                       uint32_t height, const TrueHdrParams& in) {
    const auto t = clamp(in);
    auto* p = impl_->params;
    NVSDK_NGX_Parameter_SetD3d12Resource(p, "Input1", input);
    NVSDK_NGX_Parameter_SetD3d12Resource(p, "Output", output);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.InLeft", 0);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.InTop", 0);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.InRight", width);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.InBottom", height);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.OutLeft", 0);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.OutTop", 0);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.OutRight", width);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.OutBottom", height);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.Contrast", t.contrast);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.Saturation", t.saturation);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.MiddleGray", t.middleGray);
    NVSDK_NGX_Parameter_SetUI(p, "TrueHDR.MaxLuminance", t.peakNits);
    uint32_t seh = 0;
    const auto r = guard::evaluate(list, impl_->handle, p, seh);
    if (NVSDK_NGX_FAILED(r)) {
        if (!impl_->reportedFailure)
            log::error("truehdr: evaluate failed 0x{:08X} (seh 0x{:08X})", static_cast<uint32_t>(r), seh);
        impl_->reportedFailure = true;
        return false;
    }
    return true;
}

#else

struct TrueHdr::Impl {};

std::unique_ptr<TrueHdr> TrueHdr::create(NgxRuntime*, ID3D12GraphicsCommandList*) { return nullptr; }

bool TrueHdr::evaluate(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, uint32_t, uint32_t,
                       const TrueHdrParams&) {
    return false;
}

#endif

TrueHdr::TrueHdr(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TrueHdr::~TrueHdr() = default;

} // namespace bv::ngx
