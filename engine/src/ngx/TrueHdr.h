#pragma once

#include <cstdint>
#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::ngx {

class NgxRuntime;

// Tone curve for RTX Video TrueHDR. Ranges follow the driver's accepted values.
struct TrueHdrParams {
    uint32_t contrast = 100;   // 0..200
    uint32_t saturation = 100; // 0..200
    uint32_t middleGray = 50;  // 10..100
    uint32_t peakNits = 1000;  // 400..2000 (set to the display's peak luminance)
    bool operator==(const TrueHdrParams&) const = default;
};

// RTX Video TrueHDR through NGX (official nvngx_truehdr.dll): SDR RGBA8 (sRGB) in,
// linear scRGB R16G16B16A16_FLOAT out (1.0 = 80 nits), same size.
class TrueHdr {
public:
    static std::unique_ptr<TrueHdr> create(NgxRuntime* runtime, ID3D12GraphicsCommandList* list);
    ~TrueHdr();

    // input: shader resource state; output: UNORDERED_ACCESS state.
    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, uint32_t width,
                  uint32_t height, const TrueHdrParams& params);

    static TrueHdrParams clamp(TrueHdrParams p);

    struct Impl;

private:
    explicit TrueHdr(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::ngx
