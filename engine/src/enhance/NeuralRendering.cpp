// Shaders adapted from Veyra 2.2.1 (GPL-3.0) and SAOG0721/Magpie (GPL-3.0); see docs/THIRD_PARTY_PORTS.md.
#include "NeuralRendering.h"

#include "FrameInterpolation.h"

#include <algorithm>
#include <cmath>

namespace bv::enhance {

std::array<uint32_t, 2> nrExtent(uint32_t width, uint32_t height, uint32_t size) {
    if (width == 0 || height == 0) return {0, 0};
    double scale = 1.0;
    if (size >= 125 && size <= 200) {
        scale = static_cast<double>(size - 100) / 100.0;
    } else {
        uint32_t limit = 0;
        switch (static_cast<NrSize>(size)) {
        case NrSize::P1440: limit = 1440; break;
        case NrSize::P1080: limit = 1080; break;
        case NrSize::P720: limit = 720; break;
        case NrSize::P480: limit = 480; break;
        default: limit = 0; break;
        }
        // The limit is a pixel budget of a 16:9 frame at that height, so portrait and ultrawide
        // sources get the same cost as a landscape one.
        if (limit) {
            const double budget = static_cast<double>(limit) * limit * 16.0 / 9.0;
            const double pixels = static_cast<double>(width) * height;
            if (pixels > budget) scale = std::sqrt(budget / pixels);
        }
    }
    if (scale >= 1.0) return {width, height};
    const auto w = std::max<uint32_t>(64, static_cast<uint32_t>(std::lround(width * scale)) & ~1u);
    const auto h = std::max<uint32_t>(64, static_cast<uint32_t>(std::lround(height * scale)) & ~1u);
    return {std::min(w, width), std::min(h, height)};
}

} // namespace bv::enhance

#if defined(_WIN32)

#include "core/Log.h"
#include "ngx/DlssNrParameters.h"
#include "ngx/DlssNrSession.h"
#include "ngx/NgxRuntime.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <format>

#if defined(BV_HAS_NGX)
#include <nvsdk_ngx.h>
#endif

namespace bv::enhance {

using Microsoft::WRL::ComPtr;

namespace {

// Compute passes of the NR stage. The composite is Veyra's residual composite
// (NrResidualComposite.hlsl, NrCorrection.hlsli, NrProtection.hlsli) on an sRGB-encoded RGBA8
// chain: decode to linear light, work there, encode back.
constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
    float total; float darken; float brighten; float colorAmount;
    float lumaAmount; float protectionEnabled; float featherPixels; float pad0;
    float4 regions[4];
    float correctionEnabled; float hueP; float chromaP; float highlightP;
    float compressionP; float temporalWeight; float useMotion; float pad3;
    float neutralP; float colorKeepP; float lumaKeepP; float shadowP;
    uint srcW; uint srcH; uint nrW; uint nrH;
};
// Bindings per pass (4 SRVs, 2 UAVs):
//   downsample  t0 base                                  u0 NR input
//   temporal    t0 NR input, t1 NR output, t2 history, t3 guide   u0 next history, u1 next guide
//   composite   t0 base, t1 NR input, t2 residual (history)       u0 output
Texture2D<float4> baseTex   : register(t0);
Texture2D<float4> lowTex    : register(t1);
Texture2D<float4> residualTex : register(t2);
Texture2D<float4> guideTex  : register(t3);
Texture2D<float2> motionTex : register(t4);   // temporal: shift to the previous frame (NR px)
SamplerState linearClamp : register(s0);
RWTexture2D<unorm float4> outTex : register(u0);
RWTexture2D<float4> nextHistory : register(u0);
RWTexture2D<float4> nextGuide : register(u1);

float3 decodeSrgb(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 encodeSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// ---- protection (a region with left/right swapped is the ellipse inscribed in the box)
float regionWeight(float2 pixel, uint2 size, float feather, float4 region) {
    float4 r = region * float4(size, size);
    const bool ellipse = r.z < r.x;
    if (ellipse) r = float4(r.z, r.y, r.x, r.w);
    if (r.z <= r.x || r.w <= r.y) return 0;
    float edge;
    if (ellipse) {
        const float2 centre = (r.xy + r.zw) * 0.5;
        const float2 axis = max((r.zw - r.xy) * 0.5, 1e-3);
        edge = (1.0 - length((pixel - centre) / axis)) * min(axis.x, axis.y);
    } else {
        edge = min(min(pixel.x - r.x, r.z - pixel.x), min(pixel.y - r.y, r.w - pixel.y));
    }
    return feather > 0 ? smoothstep(0, feather, edge) : (edge >= 0 ? 1 : 0);
}
float protection(float2 pixel, uint2 size) {
    float p = 0;
    if (protectionEnabled > 0) {
        [unroll] for (uint i = 0; i < 4; ++i) p = max(p, regionWeight(pixel, size, featherPixels, regions[i]));
    }
    return p;
}

// ---- correction (Oklab)
float3 toLab(float3 c) {
    float3 lms = float3(dot(c, float3(.4122214708, .5363325363, .0514459929)),
                        dot(c, float3(.2119034982, .6806995451, .1073969566)),
                        dot(c, float3(.0883024619, .2817188376, .6299787005)));
    lms = sign(lms) * pow(abs(lms), 1.0 / 3.0);
    return float3(dot(lms, float3(.2104542553, .7936177850, -.0040720468)),
                  dot(lms, float3(1.9779984951, -2.4285922050, .4505937099)),
                  dot(lms, float3(.0259040371, .7827717662, -.8086757660)));
}
float3 fromLab(float3 c) {
    float3 v = float3(c.x + .3963377774 * c.y + .2158037573 * c.z,
                      c.x - .1055613458 * c.y - .0638541728 * c.z,
                      c.x - .0894841775 * c.y - 1.2914855480 * c.z);
    v = v * v * v;
    return float3(dot(v, float3(4.0767416621, -3.3077115913, .2309699292)),
                  dot(v, float3(-1.2684380046, 2.6097574011, -.3413193965)),
                  dot(v, float3(-.0041960863, -.7034186147, 1.7076147010)));
}
bool inGamut(float3 c) { return all(c >= -1e-6) && all(c <= 1.0 + 1e-6); }
float3 mapLab(float3 lab) {
    lab.x = clamp(lab.x, 0, 1);
    float3 rgb = fromLab(lab);
    if (!inGamut(rgb)) {
        float lo = 0, hi = 1;
        [loop] for (int k = 0; k < 16; ++k) {
            float mid = (lo + hi) * .5;
            if (inGamut(fromLab(float3(lab.x, lab.yz * mid)))) lo = mid; else hi = mid;
        }
        rgb = fromLab(float3(lab.x, lab.yz * lo));
    }
    return saturate(rgb);
}
float softScale(float magnitude, float knee, float span) {
    if (magnitude <= knee) return 1;
    float excess = magnitude - knee;
    return (knee + excess / (1 + excess / span)) / max(magnitude, 1e-12);
}
float highlightShoulder(float target, float original, float limit) {
    float start = clamp(original, 0, limit);
    float room = max(limit - start, 0);
    float knee = start + room * .5;
    if (target <= knee) return target;
    float span = room * .5;
    return span > 1e-7 ? knee + span * (1 - exp(-(target - knee) / span)) : start;
}
float3 applyCorrection(float3 original, float3 candidate) {
    float hue = hueP, chroma = chromaP, highlight = highlightP, compression = compressionP;
    float neutral = neutralP, colorKeep = colorKeepP, lumaKeep = lumaKeepP, shadow = shadowP;
    if (hue + chroma + highlight + compression + neutral + colorKeep + lumaKeep + shadow == 0 || all(original == candidate))
        return candidate;
    if (!all(isfinite(candidate))) return original;
    float3 o = toLab(original), d = toLab(candidate) - o;
    float c = length(o.yz);
    if (hue > 0) {
        float weight = hue * smoothstep(0, .02, c);
        float2 n = o.yz / max(c, 1e-12), target = o.yz + d.yz;
        d.yz = lerp(target, n * max(dot(target, n), 0), weight) - o.yz;
    }
    if (chroma > 0) d.yz *= lerp(1, softScale(length(d.yz), .008 + .12 * c, .02 + .15 * c), chroma);
    if (compression > 0) d *= lerp(1, softScale(length(d), .04, .12), compression);
    if (highlight > 0 && d.x > 0) {
        float target = o.x + d.x;
        d.x = lerp(target, highlightShoulder(target, o.x, 1.0), highlight) - o.x;
    }
    d.x *= 1 - lumaKeep;
    if (d.x < 0) d.x *= 1 - shadow * (1 - smoothstep(.12, .5, o.x));
    float2 prospective = o.yz * ((o.x + d.x) / max(o.x, 1e-6));
    float tintRisk = smoothstep(.001, .004, length(o.yz + d.yz - prospective));
    float keep = 1 - (1 - colorKeep) * (1 - neutral * tintRisk * (1 - smoothstep(.04, .14, c / max(o.x, .02))));
    if (keep > 0) {
        float peak = max(original.r, max(original.g, original.b));
        float limit = o.x * pow(1.0 / max(peak, 1e-9), 1.0 / 3.0);
        d.x = lerp(o.x + d.x, min(o.x + d.x, limit), keep) - o.x;
    }
    float2 sourceChroma = o.yz * ((o.x + d.x) / max(o.x, 1e-6));
    d.yz = lerp(o.yz + d.yz, sourceChroma, keep) - o.yz;
    return mapLab(o + d);
}

// A negative residual pushed past zero keeps a tangent-matched positive curve instead of clipping.
float shadowSafe(float base, float rawDelta, float requestedDelta) {
    float anchorDelta = min(rawDelta, 0.0);
    float anchor = base + anchorDelta;
    if (base >= 0 && anchor > 0 && requestedDelta < anchorDelta)
        return anchor / (1.0 + (anchorDelta - requestedDelta) / anchor);
    return base + requestedDelta;
}

// Exact area filter, in linear light.
[numthreads(16, 16, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nrW || id.y >= nrH) return;
    float2 low = float2(id.xy) * float2(srcW, srcH) / float2(nrW, nrH);
    float2 high = float2(id.xy + 1) * float2(srcW, srcH) / float2(nrW, nrH);
    float3 sum = 0; float weights = 0;
    for (uint y = (uint)floor(low.y); y < (uint)ceil(high.y); ++y)
        for (uint x = (uint)floor(low.x); x < (uint)ceil(high.x); ++x) {
            float w = max(0, min(high.x, x + 1.0) - max(low.x, (float)x)) * max(0, min(high.y, y + 1.0) - max(low.y, (float)y));
            sum += decodeSrgb(baseTex[uint2(min(x, srcW - 1), min(y, srcH - 1))].rgb) * w;
            weights += w;
        }
    outTex[id.xy] = float4(encodeSrgb(sum / max(weights, 1e-6)), 1);
}

// NR change at NR size, stabilised over time where the picture is still. t0 = NR input,
// t1 = NR output, t2 = previous history, t3 = previous guide. The guide is the sRGB input, so the
// "still" test is perceptual; history alpha marks valid entries (textures start zeroed).
[numthreads(16, 16, 1)]
void CSTemporal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nrW || id.y >= nrH) return;
    const float3 guide = saturate(baseTex[id.xy].rgb);
    const float3 raw = decodeSrgb(lowTex[id.xy].rgb) - decodeSrgb(guide);
    float3 res = raw;
    if (temporalWeight > 0) {
        // Where this pixel was in the previous frame: same place, or along the motion.
        float2 shift = 0;
        if (useMotion > 0.5) {
            uint mw, mh;
            motionTex.GetDimensions(mw, mh);
            shift = motionTex.SampleLevel(linearClamp, (float2(id.xy) + 0.5) / (float2(mw, mh) * 8), 0);
        }
        const float2 prev = float2(id.xy) + 0.5 + shift;
        if (all(prev >= 0) && all(prev <= float2(nrW, nrH))) {
            const float2 uv = prev / float2(nrW, nrH);
            const float4 h = residualTex.SampleLevel(linearClamp, uv, 0);
            // Worst neighbour difference against the previous guide at that place: a moving edge
            // or a wrong motion vector rejects the history instead of smearing it.
            float diff = 0;
            [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
                const int2 q = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2((int)nrW - 1, (int)nrH - 1));
                const float3 g = guideTex.SampleLevel(linearClamp, (prev + float2(x, y)) / float2(nrW, nrH), 0).rgb;
                const float3 d = abs(saturate(baseTex[q].rgb) - g);
                diff = max(diff, max(d.r, max(d.g, d.b)));
            }
            const float accept = 1 - smoothstep(0.02, 0.10, diff);
            res = lerp(raw, h.rgb, temporalWeight * accept * saturate(h.a));
        }
    }
    nextHistory[id.xy] = float4(res, 1);
    nextGuide[id.xy] = float4(guide, 1);
}

[numthreads(16, 16, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID) {
    if (id.x >= srcW || id.y >= srcH) return;
    const float4 b = baseTex[id.xy];
    const float3 base = decodeSrgb(b.rgb);
    const float p = protection(float2(id.xy) + 0.5, uint2(srcW, srcH));
    if (total == 0 || p >= 1) { outTex[id.xy] = float4(b.rgb, 1); return; }

    // Edge-aware upsampling of the change; the full-resolution base is the guide.
    float2 pos = (float2(id.xy) + 0.5) * float2(nrW, nrH) / float2(srcW, srcH) - 0.5;
    int2 origin = (int2)floor(pos);
    float2 f = frac(pos);
    float3 delta = 0; float weights = 0;
    const bool scaled = srcW != nrW || srcH != nrH;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = clamp(origin + int2(x, y), int2(0, 0), int2((int)nrW - 1, (int)nrH - 1));
        float3 lowRgb = decodeSrgb(lowTex[q].rgb);
        float weight = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
        if (scaled) weight /= 1 + 16 * dot(abs(base - lowRgb), float3(0.2126, 0.7152, 0.0722));
        delta += residualTex[q].rgb * weight;
        weights += weight;
    }
    delta /= max(weights, 1e-6);
    const float3 rawDelta = delta;
    if (darken != 1 || brighten != 1) delta = min(delta, 0) * darken + max(delta, 0) * brighten;
    if (colorAmount != 1 || lumaAmount != 1) {
        float dy = dot(delta, float3(0.2126, 0.7152, 0.0722));
        delta = dy * lumaAmount + (delta - dy) * colorAmount;
    }
    delta *= total * (1 - p);
    float3 result = float3(shadowSafe(base.r, rawDelta.r, delta.r),
                           shadowSafe(base.g, rawDelta.g, delta.g),
                           shadowSafe(base.b, rawDelta.b, delta.b));
    if (correctionEnabled > 0.5) result = applyCorrection(base, result);
    outTex[id.xy] = float4(encodeSrgb(max(0, result)), 1);
}
)";

struct alignas(16) Constants {
    float total, darken, brighten, color;
    float luminance, protectionEnabled, feather, pad0;
    float regions[16];
    float correction, hue, chroma, highlight;
    float compression, temporalWeight, useMotion, pad3;
    float neutral, colorKeep, lumaKeep, shadow;
    uint32_t srcW, srcH, nrW, nrH;
};
static_assert(sizeof(Constants) == 40 * 4);

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Slots in the stage's own descriptor heap: 7 per pass (t0..t4, u0..u1). The temporal and
// composite passes have one table per history phase.
constexpr UINT kTable = 7;
constexpr UINT kSrvs = 5;
enum Slot : UINT {
    Ds = 0,
    Te0 = 7, Te1 = 14,
    Co0 = 21, Co1 = 28,
    SlotCount = 35,
};
constexpr DXGI_FORMAT kHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Automatic correction values per style (Veyra NrCorrectionSettings::automaticValues):
// hue, chroma, highlight, compression, (stability), neutral, colorKeep, lumaKeep, shadow.
std::array<float, 9> automaticCorrection(int32_t style) {
    if (style == 1) return {1, 1, 1, 1, 1, 1, .9f, .25f, .55f};
    if (style == 2) return {1, 1, 1, 1, 1, 1, .95f, .2f, .4f};
    return {1, 1, 1, 1, 1, 1, 0, 0, 0};
}

ComPtr<ID3DBlob> compile(const char* entry) {
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bv_nr", nullptr, nullptr, entry, "cs_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
        log::error("nr: shader {} failed: {}", entry,
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
        return nullptr;
    }
    return blob;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    b.UAV.pResource = r;
    return b;
}

// A texture plus the state it is in, so transitions stay correct when a frame bails out early.
struct Tracked {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    void to(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES next) {
        if (!res || state == next) return;
        const auto b = transition(res.Get(), state, next);
        list->ResourceBarrier(1, &b);
        state = next;
    }
};

} // namespace

struct NeuralRendering::Impl {
    ID3D12Device* device = nullptr;
    std::shared_ptr<ngx::DlssNrSession> session;
    uint32_t width = 0, height = 0, nrW = 0, nrH = 0;
    bool performanceModel = true;

    Tracked nrInput, neural, output;
    Tracked history[2], guide[2]; // anti-flicker ping-pong at NR size
    UINT phase = 0;                // history[phase] is written this frame
    bool reset = true;
    double frameMs = 0;
    // Motion-following anti-flicker: motion of the NR input between consecutive frames.
    std::unique_ptr<FrameInterpolation> motion;
    UINT motionSlot = 0;
    bool motionPrev = false;
    ComPtr<ID3D12Resource> zeroMotion, zeroDepth;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> downsamplePso, temporalPso, compositePso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descSize = 0;
    ID3D12Resource* boundBase = nullptr;

#if defined(BV_HAS_NGX)
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;
#endif
    bool reportedFailure = false;

    ~Impl() {
#if defined(BV_HAS_NGX)
        if (session && handle) session->releaseFeature(handle);
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
#endif
    }

    bool downsampled() const { return nrW != width || nrH != height; }

    ComPtr<ID3D12Resource> texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
        ComPtr<ID3D12Resource> tex;
        // Committed resources start zeroed: the motion and depth guides stay all zero.
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&tex))))
            return nullptr;
        return tex;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot) const {
        return {heap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(slot) * descSize};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot) const {
        return {heap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(slot) * descSize};
    }

    void srv(UINT slot, ID3D12Resource* res, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(res, &v, cpu(slot));
    }
    void uav(UINT slot, ID3D12Resource* res, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = format;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(res, nullptr, &v, cpu(slot));
    }

    bool createPasses() {
        D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants = {0, 0, sizeof(Constants) / 4};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable = {1, &srvRange};
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable = {1, &uavRange};
        for (auto& param : p) param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
        auto pso = [&](const char* entry, ComPtr<ID3D12PipelineState>& out) {
            auto cs = compile(entry);
            if (!cs) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = rootSig.Get();
            d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            return SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)));
        };
        if (!pso("CSDownsample", downsamplePso) || !pso("CSTemporal", temporalPso) || !pso("CSComposite", compositePso))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SlotCount,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // Every slot gets a valid (possibly null) view of the declared type.
        for (UINT t = 0; t < SlotCount; t += kTable) {
            for (UINT i = 0; i < 4; ++i) srv(t + i, nullptr);
            srv(t + 4, nullptr, DXGI_FORMAT_R16G16_FLOAT);
            uav(t + 5, nullptr);
            uav(t + 6, nullptr, kHistoryFormat);
        }
        if (downsampled()) uav(Ds + 5, nrInput.res.Get());
        for (UINT ph = 0; ph < 2; ++ph) {
            const UINT te = ph ? Te1 : Te0, co = ph ? Co1 : Co0;
            // Temporal phase ph writes history[ph] from history[1-ph].
            srv(te + 1, neural.res.Get());
            srv(te + 2, history[1 - ph].res.Get(), kHistoryFormat);
            srv(te + 3, guide[1 - ph].res.Get(), kHistoryFormat);
            uav(te + 5, history[ph].res.Get(), kHistoryFormat);
            uav(te + 6, guide[ph].res.Get(), kHistoryFormat);
            srv(co + 2, history[ph].res.Get(), kHistoryFormat);
            uav(co + 5, output.res.Get());
        }
        // Motion estimator for the motion-following anti-flicker: small at NR size, created once so
        // its descriptors never change while frames are in flight.
        motion = FrameInterpolation::create(device, nrW, nrH);
        if (motion)
            for (UINT t : {Te0, Te1}) srv(t + 4, motion->motion(), DXGI_FORMAT_R16G16_FLOAT);
        return true;
    }

    void bindBase(ID3D12Resource* base) {
        if (base == boundBase) return; // the presenter only replaces it after waiting for the GPU
        boundBase = base;
        ID3D12Resource* low = downsampled() ? nrInput.res.Get() : base;
        srv(Ds + 0, base);
        for (UINT t : {Te0, Te1}) srv(t + 0, low); // temporal: t0 = NR input
        for (UINT t : {Co0, Co1}) {
            srv(t + 0, base);
            srv(t + 1, low);
        }
    }

    void dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const Constants& c, UINT table, UINT w,
                  UINT h) {
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rootSig.Get());
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        list->SetComputeRootDescriptorTable(1, gpu(table));
        list->SetComputeRootDescriptorTable(2, gpu(table + kSrvs));
        list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    }

    Constants constants(const NeuralControls& n) const {
        Constants c{};
        c.total = std::clamp(n.total, 0.0f, 5.0f);
        c.darken = std::clamp(n.darken, 0.0f, 2.0f);
        c.brighten = std::clamp(n.brighten, 0.0f, 2.0f);
        c.color = std::clamp(n.color, 0.0f, 2.0f);
        c.luminance = std::clamp(n.luminance, 0.0f, 2.0f);
        c.feather = std::clamp(n.feather, 0.0f, 64.0f);
        // Regions in 0..1 of the frame: left, top, right, bottom.
        int r = 0;
        auto add = [&](float l, float t, float rr, float b) {
            if (r >= 4) return;
            c.regions[r * 4 + 0] = l;
            c.regions[r * 4 + 1] = t;
            c.regions[r * 4 + 2] = rr;
            c.regions[r * 4 + 3] = b;
            ++r;
        };
        switch (n.protect) {
        case NrProtect::Bottom: add(0, 0.80f, 1, 1); break;
        case NrProtect::Top: add(0, 0, 1, 0.12f); break;
        case NrProtect::TopAndBottom: add(0, 0.80f, 1, 1); add(0, 0, 1, 0.12f); break;
        case NrProtect::Center: add(0.44f, 0.40f, 0.56f, 0.60f); break;
        default: break;
        }
        c.protectionEnabled = r > 0 ? 1.0f : 0.0f;
        c.correction = n.correction ? 1.0f : 0.0f;
        std::array<float, 9> v{n.hue, n.chroma, n.highlight, n.compression, 0, n.neutral, n.colorKeep, n.lumaKeep, n.shadow};
        if (n.correctionAuto) {
            v = automaticCorrection(n.style);
            for (auto& x : v) x *= std::clamp(n.correctionAmount, 0.0f, 1.0f);
        }
        for (auto& x : v) x = std::clamp(x, 0.0f, 1.0f);
        c.hue = v[0];
        c.chroma = v[1];
        c.highlight = v[2];
        c.compression = v[3];
        c.neutral = v[5];
        c.colorKeep = v[6];
        c.lumaKeep = v[7];
        c.shadow = v[8];
        c.srcW = width;
        c.srcH = height;
        c.nrW = nrW;
        c.nrH = nrH;
        return c;
    }
};

std::unique_ptr<NeuralRendering> NeuralRendering::create(ngx::NgxRuntime* runtime, ID3D12Device* device,
                                                         ID3D12GraphicsCommandList* setupList, const NrLocator& locate,
                                                         uint32_t width, uint32_t height,
                                                         const NeuralControls& controls, std::string* reason) {
    auto fail = [&](std::string why) -> std::unique_ptr<NeuralRendering> {
        if (reason) *reason = std::move(why);
        return nullptr;
    };
#if !defined(BV_HAS_NGX)
    (void)runtime; (void)device; (void)setupList; (void)locate; (void)width; (void)height; (void)controls;
    return fail("Este build del motor no incluye NGX (DLSS SDK).");
#else
    if (!device || !setupList || width == 0 || height == 0) return fail("Sin dispositivo.");
    if (!runtime || !runtime->initialized())
        return fail("DLSS NR necesita una GeForce RTX con un driver de NVIDIA reciente.");

    // Pick the runtime build.
    auto variant = controls.variant;
    std::filesystem::path folder;
    if (variant == ngx::NrVariant::Auto) {
        for (auto v : {ngx::NrVariant::Compat, ngx::NrVariant::Original}) {
            folder = locate ? locate(v) : std::filesystem::path{};
            if (!folder.empty()) {
                variant = v;
                break;
            }
        }
    } else {
        folder = locate ? locate(variant) : std::filesystem::path{};
    }
    if (folder.empty()) {
        return fail(controls.variant == ngx::NrVariant::Auto
                        ? "No hay ningún nvngx_dlssnr.dll instalado. Descárgalo en Runtimes NVIDIA."
                        : std::format("Falta el runtime {}. Descárgalo en Runtimes NVIDIA.",
                                      ngx::nrVariantName(variant)));
    }

    std::string error;
    auto session = ngx::DlssNrSession::acquire(device, folder, variant, &error);
    if (!session) return fail(error);

    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->session = session;
    impl->width = width;
    impl->height = height;
    impl->performanceModel = controls.performanceModel;
    const auto extent = nrExtent(width, height, controls.size);
    impl->nrW = extent[0];
    impl->nrH = extent[1];
    const UINT nrW = impl->nrW, nrH = impl->nrH;

    // Textures: NR input (only when downsampling), NR output, the composite output, and the
    // zero motion / depth guides NR requires.
    const auto uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (impl->downsampled()) {
        impl->nrInput.res = impl->texture(nrW, nrH, DXGI_FORMAT_R8G8B8A8_UNORM, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl->nrInput.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!impl->nrInput.res) return fail("Sin memoria de video para DLSS NR.");
    }
    impl->neural.res = impl->texture(nrW, nrH, DXGI_FORMAT_R8G8B8A8_UNORM, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    impl->neural.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl->output.res = impl->texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    impl->output.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl->zeroMotion = impl->texture(nrW, nrH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    impl->zeroDepth = impl->texture(nrW, nrH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (int i = 0; i < 2; ++i) {
        impl->history[i].res = impl->texture(nrW, nrH, kHistoryFormat, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl->history[i].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        impl->guide[i].res = impl->texture(nrW, nrH, kHistoryFormat, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl->guide[i].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!impl->history[i].res || !impl->guide[i].res) return fail("Sin memoria de video para DLSS NR.");
    }
    if (!impl->neural.res || !impl->output.res || !impl->zeroMotion || !impl->zeroDepth)
        return fail("Sin memoria de video para DLSS NR.");
    if (!impl->createPasses()) return fail("No se pudieron crear los shaders de DLSS NR.");

    // Feature creation (Veyra's create contract: same size in and out, no upscaling).
    NVSDK_NGX_Parameter* create = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&create)) || !create)
        return fail("NGX no pudo reservar parámetros.");
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&impl->params)) || !impl->params) {
        NVSDK_NGX_D3D12_DestroyParameters(create);
        return fail("NGX no pudo reservar parámetros.");
    }
    namespace k = ngx::dlssnr;
    create->Set(k::kWidth, static_cast<unsigned int>(nrW));
    create->Set(k::kHeight, static_cast<unsigned int>(nrH));
    create->Set(k::kInputWidth, static_cast<unsigned int>(nrW));
    create->Set(k::kInputHeight, static_cast<unsigned int>(nrH));
    create->Set(k::kOutputWidth, static_cast<unsigned int>(nrW));
    create->Set(k::kOutputHeight, static_cast<unsigned int>(nrH));
    create->Set(k::kOutputDotWidth, static_cast<unsigned int>(nrW));
    create->Set(k::kOutputDotHeight, static_cast<unsigned int>(nrH));
    create->Set(k::kUpscaling, 0u);
    create->Set(k::kScale, 1.0f);
    create->Set(k::kScalingRatio, 1.0f);
    create->Set(k::kComputeScalingRatioCallback, ngx::DlssNrSession::scalingRatioCallback());
    create->Set(k::kHintRenderPreset, static_cast<int>(ngx::nrRenderPreset(variant, controls.performanceModel)));
    create->Set(k::kStdWidth, static_cast<unsigned int>(nrW));
    create->Set(k::kStdHeight, static_cast<unsigned int>(nrH));
    create->Set(k::kPerfQualityValue, 1);
    create->Set(k::kCreationNodeMask, 1u);
    create->Set(k::kVisibilityNodeMask, 1u);

    uint64_t result = 0;
    uint32_t seh = 0;
    const bool ok = session->createFeature(setupList, create, &impl->handle, result, seh);
    NVSDK_NGX_D3D12_DestroyParameters(create);
    if (!ok) {
        impl->handle = nullptr;
        log::warn("nr: create {}x{} failed 0x{:X} seh 0x{:08X}", nrW, nrH, result, seh);
        if (variant == ngx::NrVariant::Original)
            return fail("El runtime original de NVIDIA solo funciona en RTX 50. Usa el runtime compatible.");
        return fail(std::format("NVIDIA rechazó crear DLSS NR a {}×{} (0x{:X}).", nrW, nrH, result));
    }
    log::info("nr: {} {} model {} at {}x{} (source {}x{})", ngx::nrVariantName(variant), session->version(),
              controls.performanceModel ? "B" : "A", nrW, nrH, width, height);
    return std::unique_ptr<NeuralRendering>(new NeuralRendering(std::move(impl)));
#endif
}

NeuralRendering::NeuralRendering(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::prepare(ID3D12GraphicsCommandList* list, ID3D12Resource* base) {
    auto& im = *impl_;
    im.bindBase(base);
    if (!im.downsampled()) return;
    im.nrInput.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c{};
    c.srcW = im.width;
    c.srcH = im.height;
    c.nrW = im.nrW;
    c.nrH = im.nrH;
    im.dispatch(list, im.downsamplePso.Get(), c, Ds, im.nrW, im.nrH);
    const auto b = uavBarrier(im.nrInput.res.Get());
    list->ResourceBarrier(1, &b);
    im.nrInput.to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool NeuralRendering::evaluate(ID3D12GraphicsCommandList* list, const NeuralControls& n, bool reset, double frameMs) {
#if defined(BV_HAS_NGX)
    auto& im = *impl_;
    im.reset = reset;
    im.frameMs = frameMs;
    im.neural.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    namespace k = ngx::dlssnr;
    auto* p = im.params;
    p->Set(k::kColor, im.downsampled() ? im.nrInput.res.Get() : im.boundBase);
    p->Set(k::kOutput, im.neural.res.Get());
    p->Set(k::kMVec, im.zeroMotion.Get());
    p->Set(k::kDepth, im.zeroDepth.Get());
    const auto w = static_cast<unsigned int>(im.nrW), h = static_cast<unsigned int>(im.nrH);
    p->Set(k::kColorSubrectWidth, w);
    p->Set(k::kColorSubrectHeight, h);
    p->Set(k::kOutputSubrectWidth, w);
    p->Set(k::kOutputSubrectHeight, h);
    p->Set(k::kMVecSubrectWidth, w);
    p->Set(k::kMVecSubrectHeight, h);
    p->Set(k::kDepthSubrectWidth, w);
    p->Set(k::kDepthSubrectHeight, h);
    p->Set(k::kMVecScaleX, 1.0f);
    p->Set(k::kMVecScaleY, 1.0f);
    p->Set(k::kDepthInverted, 1);
    p->Set(k::kIndicatorInvertX, 0);
    p->Set(k::kIndicatorInvertY, 0);
    p->Set(k::kEnabled, 1);
    p->Set(k::kReset, reset ? 1 : 0);
    p->Set(k::kStyle, static_cast<int>(std::clamp(n.style, 0, 2)));
    p->Set(k::kIntensity, std::clamp(n.intensity, 0.0f, 1.0f));
    p->Set(k::kLocalToneStrength, std::clamp(n.tone, 0.0f, 1.0f));
    p->Set(k::kLocalStructureStrength, std::clamp(n.structure, 0.0f, 1.0f));
    p->Set(k::kSkinStructureStrength, n.skin < 0 ? -1.0f : std::min(n.skin, 2.0f));
    p->Set(k::kUseAutoMask, n.autoMask ? 1 : 0);
    p->Set(k::kUICorrection, n.uiCorrection ? 1 : 0);
    uint64_t result = 0;
    uint32_t seh = 0;
    if (!im.session->evaluate(list, im.handle, p, result, seh)) {
        if (!im.reportedFailure) log::error("nr: evaluate failed 0x{:X} seh 0x{:08X}", result, seh);
        im.reportedFailure = true;
        return false;
    }
    const auto b = uavBarrier(im.neural.res.Get());
    list->ResourceBarrier(1, &b);
    im.neural.to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
#else
    (void)list; (void)n; (void)reset; (void)frameMs;
    return false;
#endif
}

void NeuralRendering::composite(ID3D12GraphicsCommandList* list, ID3D12Resource* base, const NeuralControls& n) {
    auto& im = *impl_;
    im.bindBase(base);
    im.neural.to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto c = im.constants(n);

    // 1. NR change at NR size, with the anti-flicker memory: exp(-dt / tau), dropped on resets,
    //    pauses and irregular steps (seek, stall).
    const UINT ph = im.phase;
    const double tau = (n.antiFlicker == 2 || n.antiFlicker == 4) ? 180.0 : 80.0;
    const bool sane = std::isfinite(im.frameMs) && im.frameMs > 0 && im.frameMs <= 250;
    c.temporalWeight = n.antiFlicker && sane && !im.reset
                           ? static_cast<float>(std::exp(-im.frameMs / tau)) * std::clamp(n.stability, 0.0f, 1.0f)
                           : 0.0f;
    // Motion-following tiers: the shift of every NR-input pixel back to the previous frame.
    c.useMotion = 0.0f;
    if (n.antiFlicker >= 3) {
        if (im.motion) {
            ID3D12Resource* low = im.downsampled() ? im.nrInput.res.Get() : im.boundBase;
            im.motion->ingest(list, im.motionSlot, low);
            if (im.motionPrev && c.temporalWeight > 0) {
                im.motion->estimate(list, im.motionSlot, 1 - im.motionSlot);
                c.useMotion = 1.0f;
            }
            im.motionSlot = 1 - im.motionSlot;
            im.motionPrev = true;
        }
    } else {
        im.motionPrev = false; // the estimator stays (its descriptors may be in flight); just unused
    }
    im.history[1 - ph].to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    im.guide[1 - ph].to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    im.history[ph].to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    im.guide[ph].to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    im.dispatch(list, im.temporalPso.Get(), c, ph ? Te1 : Te0, im.nrW, im.nrH);
    D3D12_RESOURCE_BARRIER hb[2] = {uavBarrier(im.history[ph].res.Get()), uavBarrier(im.guide[ph].res.Get())};
    list->ResourceBarrier(2, hb);
    im.history[ph].to(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2. Composite onto the full-size picture.
    im.output.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    im.dispatch(list, im.compositePso.Get(), c, ph ? Co1 : Co0, im.width, im.height);
    im.phase = 1 - ph;
    const auto b = uavBarrier(im.output.res.Get());
    list->ResourceBarrier(1, &b);
    im.output.to(list, kRead);
    im.neural.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void NeuralRendering::finish(ID3D12GraphicsCommandList* list) {
    impl_->output.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void NeuralRendering::copyOutput(ID3D12GraphicsCommandList* list, ID3D12Resource* readback, uint64_t offset,
                                 uint32_t rowPitch) {
    auto& im = *impl_;
    im.output.to(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Offset = offset;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, im.width, im.height, 1, rowPitch};
    D3D12_TEXTURE_COPY_LOCATION src{im.output.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    im.output.to(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

ID3D12Resource* NeuralRendering::output() const { return impl_->output.res.Get(); }
uint32_t NeuralRendering::nrWidth() const { return impl_->nrW; }
uint32_t NeuralRendering::nrHeight() const { return impl_->nrH; }
bool NeuralRendering::downsampled() const { return impl_->downsampled(); }
ngx::NrVariant NeuralRendering::variant() const { return impl_->session->variant(); }

std::string NeuralRendering::label() const {
    const auto& s = *impl_->session;
    std::string out = s.variant() == ngx::NrVariant::Original ? "NVIDIA original" : "Compatible SF";
    if (!s.version().empty()) out += " " + s.version();
    if (s.variant() == ngx::NrVariant::Compat) out += impl_->performanceModel ? " · modelo B" : " · modelo A";
    if (s.ampereRoute()) out += " · ruta RTX 30";
    return out;
}

} // namespace bv::enhance

#endif // _WIN32
