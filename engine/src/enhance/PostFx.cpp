#include "PostFx.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>

namespace bv::enhance {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kShader[] = R"(
Texture2D<float4> Input : register(t0);
RWTexture2D<unorm float4> Output : register(u0);
SamplerState linearClamp : register(s0);
cbuffer Params : register(b0) {
    uint width; uint height; float brightness; float contrast;
    float saturation; float vibrance; float temperature; float tint;
    float gamma; uint fxaa; float pad0; float pad1;
};

float3 decodeSrgb(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 encodeSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// FXAA (luma edge detection, then a blend along the edge direction), on the sRGB values.
float3 antiAlias(float2 uv, float2 texel) {
    const float3 rgbM = Input.SampleLevel(linearClamp, uv, 0).rgb;
    const float lNW = luma(Input.SampleLevel(linearClamp, uv + float2(-1, -1) * texel, 0).rgb);
    const float lNE = luma(Input.SampleLevel(linearClamp, uv + float2(1, -1) * texel, 0).rgb);
    const float lSW = luma(Input.SampleLevel(linearClamp, uv + float2(-1, 1) * texel, 0).rgb);
    const float lSE = luma(Input.SampleLevel(linearClamp, uv + float2(1, 1) * texel, 0).rgb);
    const float lM = luma(rgbM);
    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return rgbM; // flat: leave it
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    const float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    const float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * texel;
    const float3 a = 0.5 * (Input.SampleLevel(linearClamp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                            Input.SampleLevel(linearClamp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    const float3 b = a * 0.5 + 0.25 * (Input.SampleLevel(linearClamp, uv - dir * 0.5, 0).rgb +
                                       Input.SampleLevel(linearClamp, uv + dir * 0.5, 0).rgb);
    const float lB = luma(b);
    return (lB < lMin || lB > lMax) ? a : b;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const float2 texel = 1.0 / float2(width, height);
    const float2 uv = (float2(id.xy) + 0.5) * texel;
    float3 c = fxaa ? antiAlias(uv, texel) : Input[id.xy].rgb;

    // Grade in linear light.
    float3 l = decodeSrgb(c) * exp2(brightness);
    // White balance: warm = more red, less blue; tint = magenta vs green.
    l *= float3(1 + 0.10 * temperature + 0.05 * tint, 1 - 0.10 * tint, 1 - 0.10 * temperature + 0.05 * tint);
    // Contrast around 18 % grey in a perceptual (gamma) domain, so shadows don't crush.
    float3 g = pow(max(l, 0), 1.0 / 2.2);
    g = (g - 0.4633) * contrast + 0.4633;
    l = pow(max(g, 0), 2.2);
    const float y = dot(l, float3(0.2126, 0.7152, 0.0722));
    const float sat = max(max(l.r, l.g), l.b) - min(min(l.r, l.g), l.b);
    const float vib = 1 + vibrance * (1 - saturate(sat * 2));
    l = y + (l - y) * saturation * vib;
    c = encodeSrgb(l);
    if (gamma != 1) c = pow(saturate(c), 1.0 / gamma);
    Output[id.xy] = float4(c, 1);
}
)";

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Constants {
    uint32_t width, height;
    float brightness, contrast, saturation, vibrance, temperature, tint, gamma;
    uint32_t fxaa;
    float pad0, pad1;
};

} // namespace

struct PostFx::Impl {
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
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants = {0, 0, sizeof(Constants) / 4};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable = {2, ranges};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{2, p, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err, cs;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
            return false;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bv_postfx", nullptr, nullptr, "main", "cs_5_1",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
            log::error("postfx: shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown");
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

std::unique_ptr<PostFx> PostFx::create(ID3D12Device* device, uint32_t width, uint32_t height) {
    if (!device || !width || !height) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->width = width;
    impl->height = height;
    if (!impl->init()) return nullptr;
    return std::unique_ptr<PostFx>(new PostFx(std::move(impl)));
}

PostFx::PostFx(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PostFx::~PostFx() = default;

void PostFx::run(ID3D12GraphicsCommandList* list, ID3D12Resource* input, const ColorControls& k) {
    auto& im = *impl_;
    if (input != im.boundInput) { // swapped only after the presenter waited for the GPU
        im.boundInput = input;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        im.device->CreateShaderResourceView(input, &v, im.heap->GetCPUDescriptorHandleForHeapStart());
    }
    const Constants c{im.width,
                      im.height,
                      std::clamp(k.brightness, -1.0f, 1.0f),
                      std::clamp(k.contrast, 0.0f, 2.0f),
                      std::clamp(k.saturation, 0.0f, 2.0f),
                      std::clamp(k.vibrance, -1.0f, 1.0f),
                      std::clamp(k.temperature, -1.0f, 1.0f),
                      std::clamp(k.tint, -1.0f, 1.0f),
                      std::clamp(k.gamma, 0.5f, 2.0f),
                      k.fxaa ? 1u : 0u,
                      0,
                      0};
    im.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {im.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(im.rootSig.Get());
    list->SetPipelineState(im.pso.Get());
    list->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
    list->SetComputeRootDescriptorTable(1, im.heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((im.width + 7) / 8, (im.height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    b.UAV.pResource = im.out.Get();
    list->ResourceBarrier(1, &b);
    im.to(list, kRead);
}

void PostFx::finish(ID3D12GraphicsCommandList* list) { impl_->to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }

void PostFx::copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset, uint32_t rowPitch) {
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
ID3D12Resource* PostFx::output() const { return impl_->out.Get(); }
uint32_t PostFx::width() const { return impl_->width; }
uint32_t PostFx::height() const { return impl_->height; }

} // namespace bv::enhance

#else

namespace bv::enhance {
struct PostFx::Impl {};
std::unique_ptr<PostFx> PostFx::create(ID3D12Device*, uint32_t, uint32_t) { return nullptr; }
PostFx::PostFx(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PostFx::~PostFx() = default;
void PostFx::run(ID3D12GraphicsCommandList*, ID3D12Resource*, const ColorControls&) {}
void PostFx::finish(ID3D12GraphicsCommandList*) {}
void PostFx::copyOutput(ID3D12GraphicsCommandList*, ID3D12Resource*, uint64_t, uint32_t) {}
ID3D12Resource* PostFx::output() const { return nullptr; }
uint32_t PostFx::width() const { return 0; }
uint32_t PostFx::height() const { return 0; }
} // namespace bv::enhance

#endif
