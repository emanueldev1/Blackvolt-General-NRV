#pragma once

#include <cstdint>
#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::ngx {

class NgxRuntime;

// RTX Video Super Resolution through NGX (official path, nvngx_vsr.dll). RGBA8 in, RGBA8
// out; the output may be up to 4x the input on each axis. Also removes compression
// artifacts at 1x. Created on the presenter's command list.
class VideoSuperResolution {
public:
    // Returns nullptr when NGX is not initialised or the feature can't be created.
    static std::unique_ptr<VideoSuperResolution> create(NgxRuntime* runtime, ID3D12GraphicsCommandList* list,
                                                        uint32_t inWidth, uint32_t inHeight, uint32_t outWidth,
                                                        uint32_t outHeight);
    ~VideoSuperResolution();

    // input: PIXEL/NON_PIXEL shader resource state; output: UNORDERED_ACCESS state.
    // quality: 1 (fastest) .. 4 (best).
    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, uint32_t inWidth, uint32_t inHeight,
                  ID3D12Resource* output, uint32_t outWidth, uint32_t outHeight, uint32_t quality);

    struct Impl;

private:
    explicit VideoSuperResolution(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::ngx
