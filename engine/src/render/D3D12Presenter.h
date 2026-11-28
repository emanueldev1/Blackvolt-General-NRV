#pragma once

#include "enhance/Denoise.h"
#include "enhance/FrameInterpolation.h"
#include "enhance/NisScaler.h"
#include "enhance/PostFx.h"
#include "enhance/NeuralRendering.h"
#include "ngx/TrueHdr.h"
#include "player/Sinks.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bv::gpu { class D3D12Context; }
namespace bv::ngx { class NgxRuntime; }
namespace bv::telemetry { class Telemetry; }
struct IDXGISwapChain1;

namespace bv::render {

// What the user asked the presenter to do with each frame.
struct EnhanceSettings {
    bool neural = false;               // NVIDIA DLSS NR (see enhance/NeuralRendering.h)
    enhance::NeuralControls neuralControls{};
    bool vsr = false;          // RTX Video Super Resolution to the on-screen size
    uint32_t vsrQuality = 4;   // 1 (fastest) .. 4 (best)
    bool trueHdr = false;      // RTX Video TrueHDR (needs Windows HDR on)
    ngx::TrueHdrParams hdr{};  // peakNits 0 = use the display's reported peak
    uint32_t interpolation = 1; // frames shown per source frame (1..4), capped by the display refresh
    uint32_t denoise = 0;       // 0 off, 1 gentle, 2 medium, 3 strong (bilateral, before NR)
    uint32_t scaler = 0;        // with vsr on: 0 RTX VSR (NIS when VSR is unavailable), 1 NVIDIA Image Scaling
    float sharpness = 0.25f;    // NIS 0..1
    enhance::ColorControls color{}; // colour controls and FXAA, after NR
    bool hdrPassthrough = true;     // HDR10/HLG sources on an HDR display stay HDR (else tone mapped)
    uint32_t compare = 0;       // A/B: 1 = original on the left half, 2 = original on the whole picture (hold V)
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
        NeuralActive = 1u << 6,
        NeuralUnavailable = 1u << 7,
        InterpolationActive = 1u << 8,
        NisActive = 1u << 9,      // NVIDIA Image Scaling instead of RTX VSR
        HdrSource = 1u << 10,     // the video is HDR10 (PQ) or HLG
        HdrPassthrough = 1u << 11, // ...and is shown as HDR (otherwise tone mapped to SDR)
    };
    uint32_t flags = 0;
    uint32_t inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
    float displayPeakNits = 0;
    // Smoothed GPU time (ms) per stage, measured with timestamp queries.
    float gpuConvertMs = 0, gpuNeuralMs = 0, gpuVsrMs = 0, gpuHdrMs = 0, gpuTotalMs = 0;
    float gpuNrPrepareMs = 0, gpuNrCompositeMs = 0, gpuPresentMs = 0;
    float gpuInterpMs = 0;            // motion estimation + warp of the last interpolated frame
    float gpuDenoiseMs = 0;
    uint64_t interpolatedFrames = 0;  // generated frames shown so far
    std::string neuralNote; // why the neural stage is not running, when requested
    uint32_t nrWidth = 0, nrHeight = 0;
    ngx::NrVariant nrVariant = ngx::NrVariant::Auto;
    std::string nrLabel;
};

// Presents decoded frames into a composition swapchain that the UI attaches to a
// WinUI SwapChainPanel. GPU frames (D3D12 Video) are sampled in place: zero copies.
// With enhancement on, the chain is YUV->RGBA8 -> [DLSS NR] -> [VSR] -> [TrueHDR -> scRGB FP16] -> screen.
class D3D12Presenter final : public player::IVideoPresenter {
public:
    static std::unique_ptr<D3D12Presenter> create(gpu::D3D12Context& gpu, ngx::NgxRuntime* ngx = nullptr,
                                                  telemetry::Telemetry* telemetry = nullptr,
                                                  enhance::NrLocator nrLocator = {});
    ~D3D12Presenter() override;

    bool present(const AVFrame& frame) override;
    void redraw() override;
    [[nodiscard]] double refreshInterval() const override;
    [[nodiscard]] unsigned interpolationFactor() const override;
    bool presentInterpolated(const AVFrame& next, float t) override;

    // Saves the next shown frame (what is on screen, letterbox excluded) as PNG. UTF-8 path.
    // Blocks until written; false when nothing is being shown or the file cannot be written.
    bool screenshot(const std::string& path);

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
