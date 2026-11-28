#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::enhance {

// NVIDIA Image Scaling (NIS SDK 1.0.3, MIT): edge-adaptive upscaling with sharpening, on any
// D3D12 GPU. 1x (sharpen only) to 2x per axis; the presenter's final pass covers the rest.
// RGBA8 (sRGB values) in and out, as the rest of the chain.
class NisScaler {
public:
    // setupList: open list for the one-time coefficient upload (executed by the caller).
    static std::unique_ptr<NisScaler> create(ID3D12Device* device, ID3D12GraphicsCommandList* setupList, uint32_t inWidth,
                                             uint32_t inHeight, uint32_t outWidth, uint32_t outHeight);
    ~NisScaler();

    // Output size for a request: the request clamped to [input, 2 x input] on each axis.
    static void outputSize(uint32_t inW, uint32_t inH, uint32_t wantW, uint32_t wantH, uint32_t& outW, uint32_t& outH);

    // input: PIXEL|NON_PIXEL shader resource state. sharpness 0..1. Leaves output() readable.
    void run(ID3D12GraphicsCommandList* list, ID3D12Resource* input, float sharpness);
    void finish(ID3D12GraphicsCommandList* list); // after every reader of output()
    [[nodiscard]] ID3D12Resource* output() const;
    [[nodiscard]] uint32_t inWidth() const;
    [[nodiscard]] uint32_t inHeight() const;
    [[nodiscard]] uint32_t outWidth() const;
    [[nodiscard]] uint32_t outHeight() const;

    struct Impl;

private:
    explicit NisScaler(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::enhance
