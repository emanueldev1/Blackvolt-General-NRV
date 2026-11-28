#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::enhance {

// Edge-preserving video denoise: 5x5 bilateral mean (Anime4K "Denoise Bilateral Mean", via
// Magpie and Veyra), with separate luma and chroma strength. Works in linear light on the RGBA8
// (sRGB) chain. One 8x8 group shares a 12x12 tile in group memory.
class Denoise {
public:
    static std::unique_ptr<Denoise> create(ID3D12Device* device, uint32_t width, uint32_t height);
    ~Denoise();

    // input: RGBA8, PIXEL|NON_PIXEL shader resource state. Leaves output() in that state too.
    // luma/chroma: 0..1 mix; range: 1 (Veyra's calibration, gentle) .. 4 (strong).
    void run(ID3D12GraphicsCommandList* list, ID3D12Resource* input, float luma, float chroma, float range = 1.0f);
    void finish(ID3D12GraphicsCommandList* list); // after every reader of output()
    // Offline use (export): copies output() into a readback buffer.
    void copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset, uint32_t rowPitch);
    [[nodiscard]] ID3D12Resource* output() const;
    [[nodiscard]] uint32_t width() const;
    [[nodiscard]] uint32_t height() const;

    struct Impl;

private:
    explicit Denoise(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::enhance
