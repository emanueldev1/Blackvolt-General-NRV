#pragma once

#include "ngx/NrArchitecturePolicy.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::ngx { class NgxRuntime; }

namespace bv::enhance {

// Internal NR resolution. NR is costly at 4K, so by default it runs at most at 1080p and only
// the change it makes is brought back to full size (edge-aware), as Veyra does.
enum class NrSize : uint32_t {
    Native = 0,
    P1440 = 1,
    P1080 = 2, // default
    P720 = 3,
    P480 = 4,
    // 100 + percent (25..100): manual scale of the source.
};

// Regions that keep the original picture (subtitles, HUD).
enum class NrProtect : uint32_t { None = 0, Bottom = 1, Top = 2, TopAndBottom = 3, Center = 4 };

// Everything the user can tune for DLSS NR. Changing variant, model or size recreates the feature;
// the rest applies on the next frame.
struct NeuralControls {
    // Runtime and model (create time)
    ngx::NrVariant variant = ngx::NrVariant::Auto;
    bool performanceModel = true; // compat build: model B (newer, faster) vs A
    uint32_t size = static_cast<uint32_t>(NrSize::P1080);

    // Model parameters (DLSSNR.*)
    int32_t style = 0;            // 0..2
    float intensity = 1.0f;       // 0..1
    float tone = 1.0f;            // local tone strength 0..1
    float structure = 1.0f;       // local structure strength 0..1
    float skin = -1.0f;           // -1 = unset, else 0..2
    bool autoMask = false;
    bool uiCorrection = false;

    // How much of NR's change is applied (residual composite)
    float total = 1.0f;     // 0..5
    float darken = 1.0f;    // 0..2
    float brighten = 1.0f;  // 0..2
    float color = 1.0f;     // 0..2
    float luminance = 1.0f; // 0..2

    // Picture correction (Oklab), on top of the residual
    bool correction = false;
    bool correctionAuto = true;
    float correctionAmount = 1.0f; // automatic strength 0..1
    float hue = 1.0f, chroma = 0.75f, highlight = 1.0f, compression = 0.75f;
    float neutral = 0.0f, colorKeep = 0.0f, lumaKeep = 0.0f, shadow = 0.0f;

    // Protected regions
    NrProtect protect = NrProtect::None;
    float feather = 8.0f; // px

    // Anti-flicker: NR's change is accumulated over time. 1 soft / 2 strong (80 / 180 ms) where the
    // picture is still; 3 soft / 4 strong also follow motion (history reprojected along the
    // optical flow of the NR input). 0 off.
    uint32_t antiFlicker = 0;
    float stability = 1.0f; // 0..1, scales the memory

    bool operator==(const NeuralControls&) const = default;

    [[nodiscard]] bool sameFeature(const NeuralControls& o) const {
        return variant == o.variant && performanceModel == o.performanceModel && size == o.size;
    }
};

// Folder that holds nvngx_dlssnr.dll for a variant (empty when that build is not installed).
using NrLocator = std::function<std::filesystem::path(ngx::NrVariant)>;

// NR internal size for a source, keeping its aspect ratio and never upscaling.
std::array<uint32_t, 2> nrExtent(uint32_t width, uint32_t height, uint32_t size);

// DLSS NR stage of the presenter:
//
//   source RGBA8 --(downsample, linear light)--> NR input --[DLSS NR]--> NR output
//   source + (NR output - NR input), upsampled edge-aware, shaped by the residual and
//   correction controls, protected regions kept --> output RGBA8 (sRGB), same size as source
//
// NR evaluation must be recorded on a command list that has not recorded anything before
// (snippet constraint), so the presenter submits its work up to prepare() first.
class NeuralRendering {
public:
    static std::unique_ptr<NeuralRendering> create(ngx::NgxRuntime* runtime, ID3D12Device* device,
                                                   ID3D12GraphicsCommandList* setupList, const NrLocator& locate,
                                                   uint32_t width, uint32_t height, const NeuralControls& controls,
                                                   std::string* reason);
    ~NeuralRendering();

    // base: RGBA8 (sRGB values), PIXEL|NON_PIXEL shader resource state, `width` x `height`.
    void prepare(ID3D12GraphicsCommandList* list, ID3D12Resource* base);
    // On a fresh command list. reset after seeks, cuts and size changes; frameMs = time since the
    // previous evaluated frame (drives the anti-flicker memory).
    bool evaluate(ID3D12GraphicsCommandList* freshList, const NeuralControls& controls, bool reset, double frameMs = 0);
    // Anti-flicker (when on) and composite. Leaves output() in PIXEL|NON_PIXEL shader resource state.
    void composite(ID3D12GraphicsCommandList* list, ID3D12Resource* base, const NeuralControls& controls);
    // After every reader of output() has been recorded: back to the write state.
    void finish(ID3D12GraphicsCommandList* list);
    // Offline use (export): copies output() into a readback buffer and returns it to the write state.
    void copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset, uint32_t rowPitch);

    [[nodiscard]] ID3D12Resource* output() const;
    [[nodiscard]] uint32_t nrWidth() const;
    [[nodiscard]] uint32_t nrHeight() const;
    [[nodiscard]] bool downsampled() const;
    [[nodiscard]] ngx::NrVariant variant() const;
    [[nodiscard]] std::string label() const; // "SF 310.9.16.0 · modelo B"

    struct Impl;

private:
    explicit NeuralRendering(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::enhance
