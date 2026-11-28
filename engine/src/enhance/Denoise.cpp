// SPDX-License-Identifier: AGPL-3.0-only
// Shader adapted from Veyra shaders/VideoDenoise.hlsl (GPL-3.0), itself from Blinue/Magpie
// Anime4K_Denoise_Bilateral_Mean.hlsl (GPL-3.0), a port of bloc97/Anime4K (MIT).
#include "Denoise.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace bv::enhance {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kShader[] = R"(
Texture2D<float4> Input : register(t0);
RWTexture2D<unorm float4> Output : register(u0);
cbuffer Params : register(b0) { uint2 extent; float lumaStrength; float chromaStrength; float range; float3 pad; };

static const uint Block = 8;
static const int Radius = 2;
static const uint Tile = Block + 2 * Radius;
static const float3 Luma709 = float3(0.2126, 0.7152, 0.0722);
static const float Spatial[5] = {0.13533528, 0.60653066, 1.0, 0.60653066, 0.13533528};
groupshared float4 pixels[Tile][Tile];

float3 decodeSrgb(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 encodeSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint thread : SV_GroupIndex) {
    const uint2 origin = group.xy * Block;
    for (uint i = thread; i < Tile * Tile; i += Block * Block) {
        const int2 at = clamp(int2(origin) + int2(i % Tile, i / Tile) - Radius, int2(0, 0), int2(extent) - 1);
        const float3 rgb = decodeSrgb(Input.Load(int3(at, 0)).rgb);
        pixels[i / Tile][i % Tile] = float4(rgb, dot(rgb, Luma709));
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 at = origin + local.xy;
    if (any(at >= extent)) return;
    const float4 center = pixels[local.y + Radius][local.x + Radius];
    // Range weights calibrated for linear SDR (white = 1).
    const float light = abs(center.w);
    // range > 1 widens the edge test: stronger smoothing of noise, edges still stop it.
    const float lumaScale = rcp((0.006 + 0.030 * light) * range);
    const float chromaScale = rcp((0.015 + 0.044 * light) * range);
    float3 sum = 0.0;
    float weights = 0.0;
    [unroll] for (uint y = 0; y < 5; ++y) {
        [unroll] for (uint x = 0; x < 5; ++x) {
            const float4 delta = pixels[local.y + y][local.x + x] - center;
            const float dy = delta.w * lumaScale;
            const float3 dc = (delta.rgb - delta.w) * chromaScale;
            const float w = Spatial[y] * Spatial[x] * exp2(-0.7213475 * (dy * dy + dot(dc, dc)));
            sum += w * delta.rgb;
            weights += w;
        }
    }
    const float3 change = sum / weights;
    const float yc = dot(change, Luma709);
    Output[at] = float4(encodeSrgb(center.rgb + chromaStrength * (change - yc) + lumaStrength * yc), 1);
}
)";

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

} // namespace

struct Denoise::Impl {
    ID3D12Device* device = nullptr;
    uint32_t width = 0, height = 0;
    ComPtr<ID3D12Resource> out;
    D3D12_RESOURCE_STATES outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descSize = 0;
    ID3D12Resource* boundInput = nullptr;

    void to(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES next) {
        if (outState == next) return;
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {out.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, outState, next};
        list->ResourceBarrier(1, &b);
        outState = next;
    }

    bool init() {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, outState, nullptr, IID_PPV_ARGS(&out))))
            return false;
        D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1};
        D3D12_DESCRIPTOR_RANGE ranges[2] = {srv, uav};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants = {0, 0, 8};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC rs{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err, cs;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
            return false;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bv_denoise", nullptr, nullptr, "main", "cs_5_1",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
            log::error("denoise: shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig.Get();
        d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(out.Get(), nullptr, &uv, {heap->GetCPUDescriptorHandleForHeapStart().ptr + descSize});
        return true;
    }
};

std::unique_ptr<Denoise> Denoise::create(ID3D12Device* device, uint32_t width, uint32_t height) {
    if (!device || !width || !height) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->width = width;
    impl->height = height;
    if (!impl->init()) return nullptr;
    return std::unique_ptr<Denoise>(new Denoise(std::move(impl)));
}

Denoise::Denoise(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Denoise::~Denoise() = default;

void Denoise::run(ID3D12GraphicsCommandList* list, ID3D12Resource* input, float luma, float chroma, float range) {
    auto& im = *impl_;
    if (input != im.boundInput) { // replaced only after the GPU went idle
        im.boundInput = input;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        im.device->CreateShaderResourceView(input, &v, im.heap->GetCPUDescriptorHandleForHeapStart());
    }
    im.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const struct { uint32_t w, h; float luma, chroma, range, pad[3]; } c{im.width, im.height, luma, chroma,
                                                                        range < 1.0f ? 1.0f : range > 4.0f ? 4.0f : range, {}};
    ID3D12DescriptorHeap* heaps[] = {im.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(im.rootSig.Get());
    list->SetPipelineState(im.pso.Get());
    list->SetComputeRoot32BitConstants(0, 8, &c, 0);
    list->SetComputeRootDescriptorTable(1, im.heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((im.width + 7) / 8, (im.height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    b.UAV.pResource = im.out.Get();
    list->ResourceBarrier(1, &b);
    im.to(list, kRead);
}

void Denoise::finish(ID3D12GraphicsCommandList* list) { impl_->to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }

void Denoise::copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset, uint32_t rowPitch) {
    auto& im = *impl_;
    im.to(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Offset = offset;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, im.width, im.height, 1, rowPitch};
    D3D12_TEXTURE_COPY_LOCATION src{im.out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    im.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
ID3D12Resource* Denoise::output() const { return impl_->out.Get(); }
uint32_t Denoise::width() const { return impl_->width; }
uint32_t Denoise::height() const { return impl_->height; }

} // namespace bv::enhance

#else

namespace bv::enhance {
struct Denoise::Impl {};
std::unique_ptr<Denoise> Denoise::create(ID3D12Device*, uint32_t, uint32_t) { return nullptr; }
Denoise::Denoise(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Denoise::~Denoise() = default;
void Denoise::run(ID3D12GraphicsCommandList*, ID3D12Resource*, float, float, float) {}
void Denoise::finish(ID3D12GraphicsCommandList*) {}
void Denoise::copyOutput(ID3D12GraphicsCommandList*, ID3D12Resource*, uint64_t, uint32_t) {}
ID3D12Resource* Denoise::output() const { return nullptr; }
uint32_t Denoise::width() const { return 0; }
uint32_t Denoise::height() const { return 0; }
} // namespace bv::enhance

#endif
