#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::enhance {

// Motion-compensated frame interpolation on any D3D12 GPU (no vendor SDK).
//
// Two frame slots at source size (RGBA8, sRGB values). ingest() copies an image into a slot and
// builds its luma pyramid (1/4 and 1/16 size) once. interpolate() estimates the motion between
// two slots with hierarchical block matching (coarse search at 1/16, candidate + refine search at
// 1/4, one vector per 8x8 block, median smoothing) and warps both frames toward time t, falling
// back to the nearer frame where the two warped samples disagree (occlusions, scene cuts).
// The motion field is cached per slot pair, so x3/x4 cost one estimation per source frame.
class FrameInterpolation {
public:
    static std::unique_ptr<FrameInterpolation> create(ID3D12Device* device, uint32_t width, uint32_t height);
    ~FrameInterpolation();

    // image: RGBA8, PIXEL|NON_PIXEL shader resource state, width x height.
    void ingest(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* image);
    // Leaves output() in PIXEL|NON_PIXEL shader resource state.
    void interpolate(ID3D12GraphicsCommandList* list, uint32_t from, uint32_t to, float t);
    // Motion only (cached per pair): afterwards motion() holds, per 8x8 block of `from`, the shift v
    // in source pixels such that slot `to` at p + v shows what slot `from` shows at p.
    void estimate(ID3D12GraphicsCommandList* list, uint32_t from, uint32_t to);
    // R16G16_FLOAT, (width + 7) / 8 x (height + 7) / 8, PIXEL|NON_PIXEL shader resource state.
    [[nodiscard]] ID3D12Resource* motion() const;
    // After every reader of output() has been recorded.
    void finish(ID3D12GraphicsCommandList* list);

    [[nodiscard]] ID3D12Resource* output() const;
    [[nodiscard]] ID3D12Resource* slot(uint32_t index) const;
    [[nodiscard]] uint32_t width() const;
    [[nodiscard]] uint32_t height() const;

    struct Impl;

private:
    explicit FrameInterpolation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::enhance
