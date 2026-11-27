#pragma once

#include "ngx/TrueHdr.h"
#include "player/Sinks.h"

#include <cstdint>
#include <memory>

namespace bv::gpu { class D3D12Context; }
namespace bv::ngx { class NgxRuntime; }
namespace bv::telemetry { class Telemetry; }
struct IDXGISwapChain1;

namespace bv::render {

// What the user asked the presenter to do with each frame.
struct EnhanceSettings {
    bool vsr = false;          // RTX Video Super Resolution to the on-screen size
    uint32_t vsrQuality = 4;   // 1 (fastest) .. 4 (best)
    bool trueHdr = false;      // RTX Video TrueHDR (needs Windows HDR on)
    ngx::TrueHdrParams hdr{};  // peakNits 0 = use the display's reported peak
    bool operator==(const EnhanceSettings&) const = default;
};

// What actually ran on the last frame, and what it cost on the GPU.
struct EnhanceStatus {
    enum Flags : uint32_t {
        VsrActive = 1u << 0,
        HdrActive = 1u << 1,
        VsrUnavailable = 1u << 2, // requested but NGX/driver refused
        HdrUnavailable = 1u << 3,
        DisplaySdr = 1u << 4,     // TrueHDR requested but Windows HDR is off
        VsrBypassed = 1u << 5,    // VSR on, but the frame is shown smaller than it is (nothing to upscale)
    };
    uint32_t flags = 0;
    uint32_t inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
    float displayPeakNits = 0;
    // Smoothed GPU time (ms) per stage, measured with timestamp queries.
    float gpuConvertMs = 0, gpuVsrMs = 0, gpuHdrMs = 0, gpuTotalMs = 0;
};

// Presents decoded frames into a composition swapchain that the UI attaches to a
// WinUI SwapChainPanel. GPU frames (D3D12 Video) are sampled in place: zero copies.
// With enhancement on, the chain is YUV->RGBA8 -> [VSR] -> [TrueHDR -> scRGB FP16] -> screen.
class D3D12Presenter final : public player::IVideoPresenter {
public:
    static std::unique_ptr<D3D12Presenter> create(gpu::D3D12Context& gpu, ngx::NgxRuntime* ngx = nullptr,
                                                  telemetry::Telemetry* telemetry = nullptr);
    ~D3D12Presenter() override;

    bool present(const AVFrame& frame) override;
    void redraw() override;
    [[nodiscard]] double refreshInterval() const override;

    // Physical pixel size of the panel and its DPI scale. Safe to call from the UI thread.
    void resize(uint32_t width, uint32_t height, float scaleX, float scaleY);
    [[nodiscard]] IDXGISwapChain1* swapChain() const;

    // Thread-safe; applied on the next frame (and the current one is redrawn).
    void setEnhance(const EnhanceSettings& settings);
    [[nodiscard]] EnhanceSettings enhance() const;
    [[nodiscard]] EnhanceStatus enhanceStatus() const;

    struct Impl;

private:
    explicit D3D12Presenter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Whether any display attached to the system is in HDR (PQ) mode, and its peak luminance.
bool displayHdrInfo(float* peakNits);

} // namespace bv::render
