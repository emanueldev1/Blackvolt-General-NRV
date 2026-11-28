#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::enhance {

// Colour controls and anti-aliasing in one pass (RGBA8 sRGB in and out, same size).
struct ColorControls {
    float brightness = 0;  // -1..1 (exposure, in stops x 1)
    float contrast = 1;    // 0..2, around mid grey
    float saturation = 1;  // 0..2
    float vibrance = 0;    // -1..1, saturation weighted toward muted colours
    float temperature = 0; // -1 (cool) .. 1 (warm)
    float tint = 0;        // -1 (green) .. 1 (magenta)
    float gamma = 1;       // 0.5..2
    bool fxaa = false;     // edge anti-aliasing (for game capture)
    bool operator==(const ColorControls&) const = default;
    [[nodiscard]] bool identity() const {
        return brightness == 0 && contrast == 1 && saturation == 1 && vibrance == 0 && temperature == 0 && tint == 0 &&
               gamma == 1 && !fxaa;
    }
};

class PostFx {
public:
    static std::unique_ptr<PostFx> create(ID3D12Device* device, uint32_t width, uint32_t height);
    ~PostFx();

    // input: PIXEL|NON_PIXEL shader resource state. Leaves output() readable.
    void run(ID3D12GraphicsCommandList* list, ID3D12Resource* input, const ColorControls& controls);
    void finish(ID3D12GraphicsCommandList* list);
    // Offline use (export): copies output() into a readback buffer.
    void copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset, uint32_t rowPitch);
    [[nodiscard]] ID3D12Resource* output() const;
    [[nodiscard]] uint32_t width() const;
    [[nodiscard]] uint32_t height() const;

    struct Impl;

private:
    explicit PostFx(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::enhance
