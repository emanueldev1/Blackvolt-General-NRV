#include "FrameInterpolation.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <utility>

namespace bv::enhance {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
    uint srcW; uint srcH; uint qW; uint qH;
    uint cW; uint cH; uint fW; uint fH;
    float t; float pad0; float pad1; float pad2;
};
Texture2D<float4> rgb0 : register(t0);
Texture2D<float4> rgb1 : register(t1);
Texture2D<float>  lum0 : register(t0);
Texture2D<float>  lum1 : register(t1);
Texture2D<float2> flowIn : register(t2);
Texture2D<float2> flowT0 : register(t0);
RWTexture2D<float4> outRgba : register(u0);
RWTexture2D<float>  outLuma : register(u0);
RWTexture2D<float2> outFlow : register(u0);
SamplerState linearClamp : register(s0);

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[numthreads(8, 8, 1)]
void CSIngest(uint3 id : SV_DispatchThreadID) {
    if (id.x >= srcW || id.y >= srcH) return;
    outRgba[id.xy] = float4(rgb0[id.xy].rgb, 1);
}

// 1/4 size luma: mean of a 4x4 block (sRGB-encoded values: closer to perceived differences).
[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID) {
    if (id.x >= qW || id.y >= qH) return;
    float s = 0;
    [unroll] for (uint y = 0; y < 4; ++y) [unroll] for (uint x = 0; x < 4; ++x)
        s += luma(rgb0[min(id.xy * 4 + uint2(x, y), uint2(srcW - 1, srcH - 1))].rgb);
    outLuma[id.xy] = s / 16;
}

// 1/16 size luma from the 1/4 level.
[numthreads(8, 8, 1)]
void CSCoarse(uint3 id : SV_DispatchThreadID) {
    if (id.x >= cW || id.y >= cH) return;
    float s = 0;
    [unroll] for (uint y = 0; y < 4; ++y) [unroll] for (uint x = 0; x < 4; ++x)
        s += lum0[min(id.xy * 4 + uint2(x, y), uint2(qW - 1, qH - 1))];
    outLuma[id.xy] = s / 16;
}

float sadCoarse(int2 c, int2 d) {
    float s = 0;
    const int2 hi = int2(cW, cH) - 1;
    [unroll] for (int y = -2; y <= 2; ++y) [unroll] for (int x = -2; x <= 2; ++x) {
        const int2 p = clamp(c + int2(x, y), 0, hi);
        s += abs(lum0[p] - lum1[clamp(p + d, 0, hi)]);
    }
    return s;
}

// Full search, +-6 coarse px (+-96 source px), small bias toward short vectors.
[numthreads(8, 8, 1)]
void CSSearchCoarse(uint3 id : SV_DispatchThreadID) {
    if (id.x >= cW || id.y >= cH) return;
    float best = 1e9; int2 bestD = 0;
    for (int dy = -6; dy <= 6; ++dy) for (int dx = -6; dx <= 6; ++dx) {
        const float s = sadCoarse(int2(id.xy), int2(dx, dy)) + 0.004 * (abs(dx) + abs(dy));
        if (s < best) { best = s; bestD = int2(dx, dy); }
    }
    outFlow[id.xy] = float2(bestD);
}

float sadFine(int2 o, int2 d) {
    float s = 0;
    const int2 hi = int2(qW, qH) - 1;
    [unroll] for (int y = -2; y <= 3; ++y) [unroll] for (int x = -2; x <= 3; ++x) {
        const int2 p = clamp(o + int2(x, y), 0, hi);
        s += abs(lum0[p] - lum1[clamp(p + d, 0, hi)]);
    }
    return s;
}

// One vector per 8x8 source block (2x2 at the 1/4 level): best of the coarse candidates around
// the block and zero, refined +-2, then a parabolic sub-pixel fit. Output in source pixels.
[numthreads(8, 8, 1)]
void CSSearchFine(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fW || id.y >= fH) return;
    const int2 o = int2(id.xy) * 2;
    const int2 c = int2(id.xy) / 2;
    const int2 hi = int2(cW, cH) - 1;
    int2 cand[6] = {
        int2(0, 0),
        int2(round(flowIn[clamp(c, 0, hi)] * 4)),
        int2(round(flowIn[clamp(c + int2(1, 0), 0, hi)] * 4)),
        int2(round(flowIn[clamp(c - int2(1, 0), 0, hi)] * 4)),
        int2(round(flowIn[clamp(c + int2(0, 1), 0, hi)] * 4)),
        int2(round(flowIn[clamp(c - int2(0, 1), 0, hi)] * 4)),
    };
    float best = 1e9; int2 bestD = 0;
    [unroll] for (int i = 0; i < 6; ++i) {
        const float s = sadFine(o, cand[i]) + 0.002 * (abs(cand[i].x) + abs(cand[i].y));
        if (s < best) { best = s; bestD = cand[i]; }
    }
    const int2 start = bestD;
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
        const int2 d = start + int2(dx, dy);
        const float s = sadFine(o, d) + 0.002 * (abs(d.x) + abs(d.y));
        if (s < best) { best = s; bestD = d; }
    }
    float2 sub = 0;
    const float l = sadFine(o, bestD - int2(1, 0)), r = sadFine(o, bestD + int2(1, 0));
    const float u = sadFine(o, bestD - int2(0, 1)), dn = sadFine(o, bestD + int2(0, 1));
    const float c0 = sadFine(o, bestD);
    const float ax = l + r - 2 * c0, ay = u + dn - 2 * c0;
    if (ax > 1e-5) sub.x = clamp(0.5 * (l - r) / ax, -0.5, 0.5);
    if (ay > 1e-5) sub.y = clamp(0.5 * (u - dn) / ay, -0.5, 0.5);
    outFlow[id.xy] = (float2(bestD) + sub) * 4;
}

float median3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
float median9(float v[9]) {
    // Median of medians: rows, then the column of row medians (close to the true median).
    return median3(median3(v[0], v[1], v[2]), median3(v[3], v[4], v[5]), median3(v[6], v[7], v[8]));
}

[numthreads(8, 8, 1)]
void CSSmooth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fW || id.y >= fH) return;
    float xs[9], ys[9];
    const int2 hi = int2(fW, fH) - 1;
    [unroll] for (int i = 0; i < 9; ++i) {
        const float2 v = flowT0[clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), 0, hi)];
        xs[i] = v.x; ys[i] = v.y;
    }
    outFlow[id.xy] = float2(median9(xs), median9(ys));
}

// Warp both frames toward t along the motion. Where the two samples disagree the motion is not
// trustworthy (occlusion, cut): take the nearer frame's sample instead of a ghosting blend.
[numthreads(8, 8, 1)]
void CSWarp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= srcW || id.y >= srcH) return;
    const float2 size = float2(srcW, srcH);
    const float2 p = float2(id.xy) + 0.5;
    const float2 v = flowIn.SampleLevel(linearClamp, p / (float2(fW, fH) * 8), 0);
    const float3 a = rgb0.SampleLevel(linearClamp, (p - t * v) / size, 0).rgb;
    const float3 b = rgb1.SampleLevel(linearClamp, (p + (1 - t) * v) / size, 0).rgb;
    const float w = smoothstep(0.06, 0.20, abs(luma(a) - luma(b)));
    outRgba[id.xy] = float4(lerp(lerp(a, b, t), t < 0.5 ? a : b, w), 1);
}
)";

struct Constants {
    uint32_t srcW, srcH, qW, qH, cW, cH, fW, fH;
    float t, pad0, pad1, pad2;
};

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

struct Tracked {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = kUav;
    void to(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES next) {
        if (!res || state == next) return;
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, next};
        list->ResourceBarrier(1, &b);
        state = next;
    }
    void written(ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        b.UAV.pResource = res.Get();
        list->ResourceBarrier(1, &b);
        to(list, kRead);
    }
};

// Descriptor tables: t0, t1, t2, u0.
enum Table : UINT {
    Ingest0, Ingest1, Luma0, Luma1, Coarse0, Coarse1,
    SearchCoarse01, SearchCoarse10, SearchFine01, SearchFine10, Smooth, Warp01, Warp10,
    TableCount
};

} // namespace

struct FrameInterpolation::Impl {
    ID3D12Device* device = nullptr;
    Constants c{};
    Tracked slots[2], lumaQ[2], lumaC[2], coarseFlow, flowRaw, flow, out;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> ingest, luma, coarse, searchCoarse, searchFine, smooth, warp;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descSize = 0;
    ID3D12Resource* ingestSource[2] = {};
    int flowFrom = -1, flowTo = -1; // pair the cached motion field belongs to
    uint64_t slotVersion[2] = {};
    uint64_t flowVersion[2] = {};

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const { return {heap->GetCPUDescriptorHandleForHeapStart().ptr + i * descSize}; }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i) const { return {heap->GetGPUDescriptorHandleForHeapStart().ptr + i * static_cast<UINT64>(descSize)}; }

    ComPtr<ID3D12Resource> texture(UINT w, UINT h, DXGI_FORMAT f) {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        ComPtr<ID3D12Resource> r;
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, kUav, nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    void srv(UINT slot, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = f;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &v, cpu(slot));
    }
    void uav(UINT slot, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = f;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &v, cpu(slot));
    }

    // One table: three SRVs (null allowed) and one UAV.
    void table(Table t, std::array<std::pair<ID3D12Resource*, DXGI_FORMAT>, 3> in, ID3D12Resource* o, DXGI_FORMAT of) {
        for (UINT i = 0; i < 3; ++i) srv(t * 4 + i, in[i].first, in[i].first ? in[i].second : DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(t * 4 + 3, o, of);
    }

    bool init(uint32_t w, uint32_t h) {
        c.srcW = w;
        c.srcH = h;
        c.qW = (w + 3) / 4;
        c.qH = (h + 3) / 4;
        c.cW = (c.qW + 3) / 4;
        c.cH = (c.qH + 3) / 4;
        c.fW = (w + 7) / 8;
        c.fH = (h + 7) / 8;
        constexpr auto rgba = DXGI_FORMAT_R8G8B8A8_UNORM, r16 = DXGI_FORMAT_R16_FLOAT, rg16 = DXGI_FORMAT_R16G16_FLOAT;
        for (int i = 0; i < 2; ++i) {
            slots[i].res = texture(w, h, rgba);
            lumaQ[i].res = texture(c.qW, c.qH, r16);
            lumaC[i].res = texture(c.cW, c.cH, r16);
            if (!slots[i].res || !lumaQ[i].res || !lumaC[i].res) return false;
        }
        coarseFlow.res = texture(c.cW, c.cH, rg16);
        flowRaw.res = texture(c.fW, c.fH, rg16);
        flow.res = texture(c.fW, c.fH, rg16);
        out.res = texture(w, h, rgba);
        if (!coarseFlow.res || !flowRaw.res || !flow.res || !out.res) return false;

        D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants = {0, 0, sizeof(Constants) / 4};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable = {1, &srvRange};
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable = {1, &uavRange};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{3, p, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
            return false;
        auto pso = [&](const char* entry, ComPtr<ID3D12PipelineState>& o) {
            ComPtr<ID3DBlob> cs, errors;
            if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bv_interp", nullptr, nullptr, entry, "cs_5_1",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &errors))) {
                log::error("interp: shader {} failed: {}", entry,
                           errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = rootSig.Get();
            d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            return SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&o)));
        };
        if (!pso("CSIngest", ingest) || !pso("CSLuma", luma) || !pso("CSCoarse", coarse) ||
            !pso("CSSearchCoarse", searchCoarse) || !pso("CSSearchFine", searchFine) || !pso("CSSmooth", smooth) ||
            !pso("CSWarp", warp))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TableCount * 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto* s0 = slots[0].res.Get();
        auto* s1 = slots[1].res.Get();
        table(Ingest0, {{{nullptr, rgba}, {nullptr, rgba}, {nullptr, rgba}}}, s0, rgba);
        table(Ingest1, {{{nullptr, rgba}, {nullptr, rgba}, {nullptr, rgba}}}, s1, rgba);
        table(Luma0, {{{s0, rgba}, {nullptr, rgba}, {nullptr, rgba}}}, lumaQ[0].res.Get(), r16);
        table(Luma1, {{{s1, rgba}, {nullptr, rgba}, {nullptr, rgba}}}, lumaQ[1].res.Get(), r16);
        table(Coarse0, {{{lumaQ[0].res.Get(), r16}, {nullptr, rgba}, {nullptr, rgba}}}, lumaC[0].res.Get(), r16);
        table(Coarse1, {{{lumaQ[1].res.Get(), r16}, {nullptr, rgba}, {nullptr, rgba}}}, lumaC[1].res.Get(), r16);
        table(SearchCoarse01, {{{lumaC[0].res.Get(), r16}, {lumaC[1].res.Get(), r16}, {nullptr, rgba}}}, coarseFlow.res.Get(), rg16);
        table(SearchCoarse10, {{{lumaC[1].res.Get(), r16}, {lumaC[0].res.Get(), r16}, {nullptr, rgba}}}, coarseFlow.res.Get(), rg16);
        table(SearchFine01, {{{lumaQ[0].res.Get(), r16}, {lumaQ[1].res.Get(), r16}, {coarseFlow.res.Get(), rg16}}}, flowRaw.res.Get(), rg16);
        table(SearchFine10, {{{lumaQ[1].res.Get(), r16}, {lumaQ[0].res.Get(), r16}, {coarseFlow.res.Get(), rg16}}}, flowRaw.res.Get(), rg16);
        table(Smooth, {{{flowRaw.res.Get(), rg16}, {nullptr, rgba}, {nullptr, rgba}}}, flow.res.Get(), rg16);
        table(Warp01, {{{s0, rgba}, {s1, rgba}, {flow.res.Get(), rg16}}}, out.res.Get(), rgba);
        table(Warp10, {{{s1, rgba}, {s0, rgba}, {flow.res.Get(), rg16}}}, out.res.Get(), rgba);
        return true;
    }

    void dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, Table t, UINT w, UINT h) {
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rootSig.Get());
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        list->SetComputeRootDescriptorTable(1, gpu(t * 4));
        list->SetComputeRootDescriptorTable(2, gpu(t * 4 + 3));
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
};

std::unique_ptr<FrameInterpolation> FrameInterpolation::create(ID3D12Device* device, uint32_t width, uint32_t height) {
    if (!device || width < 16 || height < 16) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    if (!impl->init(width, height)) {
        log::warn("interp: could not create the {}x{} interpolation stage", width, height);
        return nullptr;
    }
    return std::unique_ptr<FrameInterpolation>(new FrameInterpolation(std::move(impl)));
}

FrameInterpolation::FrameInterpolation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameInterpolation::~FrameInterpolation() = default;

void FrameInterpolation::ingest(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* image) {
    auto& im = *impl_;
    slot &= 1;
    if (im.ingestSource[slot] != image) { // the presenter swaps images only after waiting for the GPU
        im.ingestSource[slot] = image;
        im.srv((slot ? Ingest1 : Ingest0) * 4, image, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    im.slots[slot].to(list, kUav);
    im.dispatch(list, im.ingest.Get(), slot ? Ingest1 : Ingest0, im.c.srcW, im.c.srcH);
    im.slots[slot].written(list);
    im.lumaQ[slot].to(list, kUav);
    im.dispatch(list, im.luma.Get(), slot ? Luma1 : Luma0, im.c.qW, im.c.qH);
    im.lumaQ[slot].written(list);
    im.lumaC[slot].to(list, kUav);
    im.dispatch(list, im.coarse.Get(), slot ? Coarse1 : Coarse0, im.c.cW, im.c.cH);
    im.lumaC[slot].written(list);
    ++im.slotVersion[slot];
}

void FrameInterpolation::interpolate(ID3D12GraphicsCommandList* list, uint32_t from, uint32_t to, float t) {
    auto& im = *impl_;
    from &= 1;
    to &= 1;
    const bool forward = from == 0;
    estimate(list, from, to);
    im.c.t = std::clamp(t, 0.0f, 1.0f);
    im.out.to(list, kUav);
    im.dispatch(list, im.warp.Get(), forward ? Warp01 : Warp10, im.c.srcW, im.c.srcH);
    im.out.written(list);
}

void FrameInterpolation::estimate(ID3D12GraphicsCommandList* list, uint32_t from, uint32_t to) {
    auto& im = *impl_;
    from &= 1;
    to &= 1;
    const bool forward = from == 0;
    // Motion once per pair of ingested frames; every extra t only warps.
    if (im.flowFrom != static_cast<int>(from) || im.flowTo != static_cast<int>(to) ||
        im.flowVersion[0] != im.slotVersion[0] || im.flowVersion[1] != im.slotVersion[1]) {
        im.coarseFlow.to(list, kUav);
        im.dispatch(list, im.searchCoarse.Get(), forward ? SearchCoarse01 : SearchCoarse10, im.c.cW, im.c.cH);
        im.coarseFlow.written(list);
        im.flowRaw.to(list, kUav);
        im.dispatch(list, im.searchFine.Get(), forward ? SearchFine01 : SearchFine10, im.c.fW, im.c.fH);
        im.flowRaw.written(list);
        im.flow.to(list, kUav);
        im.dispatch(list, im.smooth.Get(), Smooth, im.c.fW, im.c.fH);
        im.flow.written(list);
        im.flowFrom = static_cast<int>(from);
        im.flowTo = static_cast<int>(to);
        im.flowVersion[0] = im.slotVersion[0];
        im.flowVersion[1] = im.slotVersion[1];
    }
}

ID3D12Resource* FrameInterpolation::motion() const { return impl_->flow.res.Get(); }

void FrameInterpolation::finish(ID3D12GraphicsCommandList* list) { impl_->out.to(list, kUav); }

ID3D12Resource* FrameInterpolation::output() const { return impl_->out.res.Get(); }
ID3D12Resource* FrameInterpolation::slot(uint32_t index) const { return impl_->slots[index & 1].res.Get(); }
uint32_t FrameInterpolation::width() const { return impl_->c.srcW; }
uint32_t FrameInterpolation::height() const { return impl_->c.srcH; }

} // namespace bv::enhance

#else

namespace bv::enhance {
struct FrameInterpolation::Impl {};
std::unique_ptr<FrameInterpolation> FrameInterpolation::create(ID3D12Device*, uint32_t, uint32_t) { return nullptr; }
FrameInterpolation::FrameInterpolation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameInterpolation::~FrameInterpolation() = default;
void FrameInterpolation::ingest(ID3D12GraphicsCommandList*, uint32_t, ID3D12Resource*) {}
void FrameInterpolation::interpolate(ID3D12GraphicsCommandList*, uint32_t, uint32_t, float) {}
void FrameInterpolation::estimate(ID3D12GraphicsCommandList*, uint32_t, uint32_t) {}
ID3D12Resource* FrameInterpolation::motion() const { return nullptr; }
void FrameInterpolation::finish(ID3D12GraphicsCommandList*) {}
ID3D12Resource* FrameInterpolation::output() const { return nullptr; }
ID3D12Resource* FrameInterpolation::slot(uint32_t) const { return nullptr; }
uint32_t FrameInterpolation::width() const { return 0; }
uint32_t FrameInterpolation::height() const { return 0; }
} // namespace bv::enhance

#endif
