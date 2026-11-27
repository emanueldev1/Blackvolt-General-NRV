#include "D3D12Presenter.h"

#if defined(_WIN32)

#include "ColorMatrix.h"
#include "core/Log.h"
#include "gpu/D3D12Context.h"
#include "ngx/NgxRuntime.h"
#include "ngx/VideoSuperResolution.h"
#include "telemetry/Telemetry.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
#include <libswscale/swscale.h>
}

namespace bv::render {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kBackBuffers = 3;
constexpr DXGI_FORMAT kSdrFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kRgbaFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // what VSR and TrueHDR consume
constexpr UINT kSrvPerPass = 3;
constexpr UINT kSrvPerFrame = 2 * kSrvPerPass; // pass A (decode -> RGB) + final pass
constexpr UINT kStamps = 6;                    // start, converted, neural, vsr, hdr, end
constexpr double kSmoothing = 0.1;

// Fullscreen triangle.
// mode 0: YUV planes -> RGB with a matrix.  mode 1: RGB texture as is.
// mode 2: sRGB texture -> linear scRGB (an SDR frame shown on the FP16 swapchain).
constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float4 row0; float4 row1; float4 row2; float4 uvScale; uint mode; };
Texture2D<float>  lumaTex   : register(t0);
Texture2D<float2> chromaTex : register(t1);
Texture2D<float4> rgbTex    : register(t2);
SamplerState linearSampler  : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv * uvScale.xy;
    return o;
}

float3 srgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PSMain(VSOut i) : SV_Target {
    if (mode == 1) return float4(rgbTex.Sample(linearSampler, i.uv).rgb, 1);
    if (mode == 2) return float4(srgbToLinear(saturate(rgbTex.Sample(linearSampler, i.uv).rgb)), 1);
    float4 yuv = float4(lumaTex.Sample(linearSampler, i.uv), chromaTex.Sample(linearSampler, i.uv), 1);
    return float4(saturate(float3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv))), 1);
}
)";

struct alignas(16) Params {
    std::array<float, 12> matrix;
    float uvScale[4];
    uint32_t mode;
    uint32_t pad[3];
};

YuvMatrix matrixFor(const AVFrame& f) {
    switch (f.colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return YuvMatrix::Bt601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return YuvMatrix::Bt2020;
    case AVCOL_SPC_BT709: return YuvMatrix::Bt709;
    default: return f.height <= 576 ? YuvMatrix::Bt601 : YuvMatrix::Bt709;
    }
}

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bv_present", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
        log::error("shader compile failed: {}",
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

const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void smooth(float& slot, double value) {
    slot = slot <= 0 ? static_cast<float>(value) : static_cast<float>(slot + (value - slot) * kSmoothing);
}

} // namespace

bool displayHdrInfo(float* peakNits) {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    bool hdr = false;
    float peak = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> out6;
            DXGI_OUTPUT_DESC1 desc{};
            if (SUCCEEDED(output.As(&out6)) && SUCCEEDED(out6->GetDesc1(&desc)) &&
                desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                hdr = true;
                peak = std::max(peak, desc.MaxLuminance);
            }
        }
    }
    if (peakNits) *peakNits = peak;
    return hdr;
}

struct D3D12Presenter::Impl {
    gpu::D3D12Context* gpu = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ngx::NgxRuntime* ngx = nullptr;
    telemetry::StageStats* convertStage = nullptr;
    telemetry::StageStats* vsrStage = nullptr;
    telemetry::StageStats* hdrStage = nullptr;
    telemetry::StageStats* gpuStage = nullptr;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGISwapChain3> swapChain;
    HANDLE frameLatency = nullptr;
    bool tearing = false;
    UINT width = 64, height = 64;
    DXGI_FORMAT bbFormat = kSdrFormat;

    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    UINT rtvSize = 0, srvSize = 0;
    std::array<ComPtr<ID3D12Resource>, kBackBuffers> backBuffers;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3DBlob> vsBlob, psBlob;
    ComPtr<ID3D12PipelineState> psoSdr, psoHdr, psoRgba;

    std::array<ComPtr<ID3D12CommandAllocator>, kBackBuffers> allocators;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> setupAllocator;
    ComPtr<ID3D12GraphicsCommandList> setupList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    std::array<UINT64, kBackBuffers> frameFence{};
    // Frames the GPU may still be reading; released once their fence completes.
    std::array<AVFrame*, kBackBuffers> inFlight{};

    // GPU timing.
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> queryReadback;
    std::array<bool, kBackBuffers> timed{};
    double tickMs = 0;

    // Software fallback: frames converted to BGRA and uploaded.
    SwsContext* sws = nullptr;
    ComPtr<ID3D12Resource> rgbTex, upload;
    UINT rgbW = 0, rgbH = 0;

    // Enhancement chain. One set of intermediates is enough: all work runs in order on
    // one queue and the swapchain lets at most one frame queue up.
    EnhanceSettings settings;
    EnhanceStatus status;
    bool vsrUnavailable = false, hdrUnavailable = false;
    bool hdrDisplay = false;
    float hdrPeak = 0;
    ComPtr<ID3D12Resource> srcRgb, vsrOut, hdrOut;
    UINT srcW = 0, srcH = 0, vsrW = 0, vsrH = 0, hdrW = 0, hdrH = 0;
    std::unique_ptr<ngx::VideoSuperResolution> vsr;
    UINT vsrInW = 0, vsrInH = 0, vsrOutW = 0, vsrOutH = 0;
    std::unique_ptr<ngx::TrueHdr> trueHdr;
    std::unique_ptr<enhance::NeuralRendering> neural;
    bool neuralUnavailable = false;
    std::string neuralNote;
    ComPtr<ID3D12Resource> neuralOut;
    UINT neuralW = 0, neuralH = 0;
    int64_t lastPts = AV_NOPTS_VALUE;

    AVFrame* last = nullptr; // for redraw()
    mutable std::mutex mutex;

    ~Impl() {
        waitIdle();
        neural.reset();
        vsr.reset();
        trueHdr.reset();
        for (auto*& f : inFlight) av_frame_free(&f);
        av_frame_free(&last);
        sws_freeContext(sws);
        if (fenceEvent) CloseHandle(fenceEvent);
        if (frameLatency) CloseHandle(frameLatency);
    }

    void waitIdle() {
        if (!fence) return;
        const UINT64 v = ++fenceValue;
        queue->Signal(fence.Get(), v);
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, 2000);
        }
    }

    UINT swapFlags() const {
        return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    }

    bool init() {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        BOOL allow = FALSE;
        tearing = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = kSdrFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Flags = swapFlags();
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForComposition(queue, &sd, nullptr, &sc1)) || FAILED(sc1.As(&swapChain))) {
            log::error("presenter: CreateSwapChainForComposition failed");
            return false;
        }
        swapChain->SetMaximumFrameLatency(1); // lowest latency: render only when a buffer is free
        frameLatency = swapChain->GetFrameLatencyWaitableObject();

        // Back buffers + the RGBA8 intermediate that pass A renders into.
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBuffers + 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap));
        rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        // Per back buffer: 3 SRVs for pass A and 3 for the final pass, so in-flight frames keep theirs.
        D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvPerFrame * kBackBuffers,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap));
        srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (auto& a : allocators) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&setupAllocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, setupAllocator.Get(), nullptr,
                                  IID_PPV_ARGS(&setupList));
        setupList->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kBackBuffers, 0};
        UINT64 freq = 0;
        if (SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queryHeap))) && SUCCEEDED(queue->GetTimestampFrequency(&freq)) &&
            freq) {
            tickMs = 1000.0 / static_cast<double>(freq);
            D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof(UINT64) * kStamps * kBackBuffers, 1, 1, 1,
                                   DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            if (FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&queryReadback)))) {
                queryHeap.Reset();
            }
        } else {
            queryHeap.Reset();
        }

        if (!createPipeline()) return false;
        createBackBufferViews();
        hdrDisplay = displayHdrInfo(&hdrPeak);
        return true;
    }

    bool createPipeline() {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvPerPass, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, sizeof(Params) / 4};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) {
            log::error("presenter: root signature failed");
            return false;
        }

        vsBlob = compile("VSMain", "vs_5_1");
        psBlob = compile("PSMain", "ps_5_1");
        if (!vsBlob || !psBlob) return false;
        psoSdr = makePso(kSdrFormat);
        return psoSdr != nullptr;
    }

    ComPtr<ID3D12PipelineState> makePso(DXGI_FORMAT target) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig.Get();
        pd.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
        pd.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = target;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            log::error("presenter: PSO for format {} failed", static_cast<int>(target));
            return nullptr;
        }
        return pso;
    }

    ID3D12PipelineState* psoFor(DXGI_FORMAT target) {
        auto& slot = target == kHdrFormat ? psoHdr : target == kRgbaFormat ? psoRgba : psoSdr;
        if (!slot) slot = makePso(target);
        return slot.Get();
    }

    void createBackBufferViews() {
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i]));
            device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, {h.ptr + i * rtvSize});
        }
    }

    // Recreates the back buffers at a new size and/or format. Caller holds the mutex.
    void rebuildSwapchain(UINT w, UINT h, DXGI_FORMAT format) {
        w = std::max<UINT>(w, 8);
        h = std::max<UINT>(h, 8);
        if (w == width && h == height && format == bbFormat) return;
        waitIdle();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            backBuffers[i].Reset();
            av_frame_free(&inFlight[i]);
            frameFence[i] = 0;
            timed[i] = false;
        }
        if (FAILED(swapChain->ResizeBuffers(kBackBuffers, w, h, format, swapFlags()))) {
            log::error("presenter: ResizeBuffers({}x{}, format {}) failed", w, h, static_cast<int>(format));
        } else {
            if (format != bbFormat) {
                const auto space = format == kHdrFormat ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                UINT support = 0;
                if (SUCCEEDED(swapChain->CheckColorSpaceSupport(space, &support)) &&
                    (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                    swapChain->SetColorSpace1(space);
                }
                log::info("presenter: swapchain now {}", format == kHdrFormat ? "scRGB FP16 (HDR)" : "BGRA8 (SDR)");
            }
            width = w;
            height = h;
            bbFormat = format;
        }
        createBackBufferViews();
    }

    ComPtr<ID3D12Resource> makeTexture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                       D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
        ComPtr<ID3D12Resource> tex;
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&tex)))) {
            log::error("presenter: texture {}x{} format {} failed", w, h, static_cast<int>(format));
            return nullptr;
        }
        return tex;
    }

    // Runs one-off work (NGX feature creation) outside the frame and waits for it.
    template <typename F>
    auto runSetup(F&& fn) {
        setupAllocator->Reset();
        setupList->Reset(setupAllocator.Get(), nullptr);
        auto result = fn(setupList.Get());
        setupList->Close();
        ID3D12CommandList* lists[] = {setupList.Get()};
        queue->ExecuteCommandLists(1, lists);
        waitIdle();
        return result;
    }

    // Decides what runs for this frame and makes sure the features and textures exist.
    // Called before a back buffer is acquired, because it may recreate the swapchain.
    struct Plan {
        bool enhanced = false, neural = false, vsr = false, hdr = false;
        UINT outW = 0, outH = 0;
        Viewport vp{};
    };

    Plan plan(const AVFrame& frame) {
        Plan p;
        const auto fit = fitAspect(static_cast<float>(frame.width), static_cast<float>(frame.height),
                                   frame.sample_aspect_ratio.num ? static_cast<float>(av_q2d(frame.sample_aspect_ratio)) : 1.0f,
                                   static_cast<float>(width), static_cast<float>(height));
        p.vp = fit;
        status.flags = 0;
        status.inputWidth = static_cast<uint32_t>(frame.width);
        status.inputHeight = static_cast<uint32_t>(frame.height);

        // Snap the picture to whole pixels so the enhanced image is drawn 1:1.
        p.outW = std::max<UINT>(1, static_cast<UINT>(std::lround(fit.width)));
        p.outH = std::max<UINT>(1, static_cast<UINT>(std::lround(fit.height)));
        const UINT inW = static_cast<UINT>(frame.width), inH = static_cast<UINT>(frame.height);

        if (settings.trueHdr) {
            if (factory && !factory->IsCurrent()) { // display configuration changed
                factory.Reset();
                CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
                hdrDisplay = displayHdrInfo(&hdrPeak);
            }
            status.displayPeakNits = hdrPeak;
            if (!hdrDisplay) status.flags |= EnhanceStatus::DisplaySdr;
        }

        // Neural rendering at source size, first in the chain (no implementation ships yet).
        p.neural = settings.neural && !neuralUnavailable;
        if (p.neural && (!neural || neuralW != inW || neuralH != inH)) {
            waitIdle();
            neural.reset();
            neuralNote.clear();
            neural = runSetup([&](ID3D12GraphicsCommandList* cl) {
                return enhance::createNeuralRendering(ngx, cl, inW, inH, &neuralNote);
            });
            if (!neural) {
                neuralUnavailable = true;
                p.neural = false;
                log::info("neural rendering not available: {}", neuralNote);
            }
        }
        if (p.neural && !ensureTexture(neuralOut, neuralW, neuralH, inW, inH, kRgbaFormat,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            p.neural = false;
        }
        if (settings.neural && neuralUnavailable) status.flags |= EnhanceStatus::NeuralUnavailable;
        status.neuralNote = settings.neural && neuralUnavailable ? neuralNote : std::string();

        p.vsr = settings.vsr && ngx && !vsrUnavailable;
        if (p.vsr && (p.outW < inW || p.outH < inH || p.outW > 4 * inW || p.outH > 4 * inH)) {
            // Shown smaller than decoded (or beyond VSR's 4x limit): bilinear is the right tool.
            p.vsr = false;
            status.flags |= EnhanceStatus::VsrBypassed;
        }
        p.hdr = settings.trueHdr && ngx && hdrDisplay && !hdrUnavailable;

        if (p.vsr && (!vsr || vsrInW != inW || vsrInH != inH || vsrOutW != p.outW || vsrOutH != p.outH)) {
            waitIdle();
            vsr.reset();
            vsr = runSetup([&](ID3D12GraphicsCommandList* cl) {
                return ngx::VideoSuperResolution::create(ngx, cl, inW, inH, p.outW, p.outH);
            });
            vsrInW = inW;
            vsrInH = inH;
            vsrOutW = p.outW;
            vsrOutH = p.outH;
            if (!vsr) {
                vsrUnavailable = true;
                p.vsr = false;
            }
        }
        if (p.hdr && !trueHdr) {
            waitIdle();
            trueHdr = runSetup([&](ID3D12GraphicsCommandList* cl) { return ngx::TrueHdr::create(ngx, cl); });
            if (!trueHdr) {
                hdrUnavailable = true;
                p.hdr = false;
            }
        }
        if (settings.vsr && vsrUnavailable) status.flags |= EnhanceStatus::VsrUnavailable;
        if (settings.trueHdr && hdrUnavailable) status.flags |= EnhanceStatus::HdrUnavailable;

        p.enhanced = p.neural || p.vsr || p.hdr;
        if (p.enhanced) {
            if (!ensureTexture(srcRgb, srcW, srcH, inW, inH, kRgbaFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kShaderRead)) {
                p.enhanced = p.neural = p.vsr = p.hdr = false;
            } else {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + kBackBuffers * rtvSize};
                device->CreateRenderTargetView(srcRgb.Get(), nullptr, rtv);
            }
        }
        if (p.vsr && !ensureTexture(vsrOut, vsrW, vsrH, p.outW, p.outH, kRgbaFormat,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            p.vsr = false;
        }
        const UINT hdrInW = p.vsr ? p.outW : inW, hdrInH = p.vsr ? p.outH : inH;
        if (p.hdr && !ensureTexture(hdrOut, hdrW, hdrH, hdrInW, hdrInH, kHdrFormat,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            p.hdr = false;
        }
        p.enhanced = p.neural || p.vsr || p.hdr;

        rebuildSwapchain(width, height, p.hdr ? kHdrFormat : kSdrFormat);
        if (p.neural) status.flags |= EnhanceStatus::NeuralActive;
        if (p.vsr) status.flags |= EnhanceStatus::VsrActive;
        if (p.hdr) status.flags |= EnhanceStatus::HdrActive;
        status.outputWidth = p.enhanced ? (p.vsr ? p.outW : inW) : p.outW;
        status.outputHeight = p.enhanced ? (p.vsr ? p.outH : inH) : p.outH;
        return p;
    }

    bool ensureTexture(ComPtr<ID3D12Resource>& tex, UINT& curW, UINT& curH, UINT w, UINT h, DXGI_FORMAT format,
                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        if (tex && curW == w && curH == h) return true;
        waitIdle();
        tex = makeTexture(w, h, format, flags, state);
        curW = tex ? w : 0;
        curH = tex ? h : 0;
        return tex != nullptr;
    }

    // Software frames: convert to BGRA on the CPU and upload. Only used when GPU decode is
    // unavailable for the codec, so simplicity wins over speed here.
    bool uploadSoftware(const AVFrame& f, ID3D12GraphicsCommandList* cl) {
        const UINT w = static_cast<UINT>(f.width), h = static_cast<UINT>(f.height);
        if (!rgbTex || rgbW != w || rgbH != h) {
            waitIdle();
            rgbTex = makeTexture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!rgbTex) return false;
            const auto td = rgbTex->GetDesc();
            UINT64 total = 0;
            device->GetCopyableFootprints(&td, 0, 1, 0, nullptr, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total * kBackBuffers, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload));
            rgbW = w;
            rgbH = h;
        } else {
            const auto b = transition(rgbTex.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->ResourceBarrier(1, &b);
        }

        const auto td = rgbTex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        const UINT slot = swapChain->GetCurrentBackBufferIndex();
        fp.Offset = total * slot;

        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        sws = sws_getCachedContext(sws, f.width, f.height, static_cast<AVPixelFormat>(f.format), f.width, f.height,
                                   AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        uint8_t* dst[1] = {mapped + fp.Offset};
        int dstStride[1] = {static_cast<int>(fp.Footprint.RowPitch)};
        sws_scale(sws, f.data, f.linesize, 0, f.height, dst, dstStride);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION dstLoc{rgbTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dstLoc.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &src, nullptr);
        const auto b = transition(rgbTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
        cl->ResourceBarrier(1, &b);
        return true;
    }

    void stamp(UINT bb, UINT index) {
        if (queryHeap) list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, bb * kStamps + index);
    }

    // Reads the timestamps of the frame that last used this slot (its fence has completed).
    void collectTimings(UINT bb) {
        if (!queryHeap || !timed[bb]) return;
        timed[bb] = false;
        const D3D12_RANGE range{bb * kStamps * sizeof(UINT64), (bb + 1) * kStamps * sizeof(UINT64)};
        UINT64* data = nullptr;
        if (FAILED(queryReadback->Map(0, &range, reinterpret_cast<void**>(&data)))) return;
        const UINT64* t = data + bb * kStamps;
        const auto ms = [&](UINT a, UINT b) { return t[b] > t[a] ? static_cast<double>(t[b] - t[a]) * tickMs : 0.0; };
        const double convert = ms(0, 1), neuralMs = ms(1, 2), vsrMs = ms(2, 3), hdrMs = ms(3, 4), total = ms(0, 5);
        const D3D12_RANGE none{0, 0};
        queryReadback->Unmap(0, &none);
        smooth(status.gpuConvertMs, convert);
        smooth(status.gpuNeuralMs, neuralMs);
        smooth(status.gpuVsrMs, vsrMs);
        smooth(status.gpuHdrMs, hdrMs);
        smooth(status.gpuTotalMs, total);
        if (gpuStage) gpuStage->record(total * 1000.0);
        if (convertStage) convertStage->record(convert * 1000.0);
        if (vsrStage && vsrMs > 0) vsrStage->record(vsrMs * 1000.0);
        if (hdrStage && hdrMs > 0) hdrStage->record(hdrMs * 1000.0);
    }

    // Binds the source frame into the 3 SRV slots at `cpu` and fills the shader params.
    void bindSource(const AVFrame& frame, D3D12_CPU_DESCRIPTOR_HANDLE cpu, Params& p) {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
        nullDesc.Format = DXGI_FORMAT_R8_UNORM;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;
        p.uvScale[0] = p.uvScale[1] = 1.0f;

        if (frame.format == AV_PIX_FMT_D3D12) {
            auto* hw = reinterpret_cast<AVD3D12VAFrame*>(frame.data[0]);
            ID3D12Resource* tex = hw->texture;
            // Do not sample before the decoder finished writing the surface.
            queue->Wait(hw->sync_ctx.fence, hw->sync_ctx.fence_value);

            const auto desc = tex->GetDesc();
            const bool tenBit = desc.Format == DXGI_FORMAT_P010 || desc.Format == DXGI_FORMAT_P016;
            D3D12_SHADER_RESOURCE_VIEW_DESC luma = nullDesc, chroma = nullDesc;
            luma.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            chroma.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
            chroma.Texture2D.PlaneSlice = 1;
            device->CreateShaderResourceView(tex, &luma, cpu);
            device->CreateShaderResourceView(tex, &chroma, {cpu.ptr + srvSize});
            device->CreateShaderResourceView(nullptr, &nullDesc, {cpu.ptr + 2 * srvSize});
            // Decoder surfaces are often padded (e.g. 1088 lines); sample only the visible part.
            p.uvScale[0] = static_cast<float>(frame.width) / static_cast<float>(desc.Width);
            p.uvScale[1] = static_cast<float>(frame.height) / static_cast<float>(desc.Height);
            p.matrix = yuvToRgb(matrixFor(frame), frame.color_range == AVCOL_RANGE_JPEG);
            p.mode = 0;
        } else {
            uploadSoftware(frame, list.Get());
            D3D12_SHADER_RESOURCE_VIEW_DESC rgb = nullDesc;
            rgb.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bindRgb(cpu, rgbTex.Get(), rgb.Format);
            p.mode = 1;
        }
    }

    // Binds an RGB texture into slot 2 (slots 0/1 get null views).
    void bindRgb(D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* tex, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        v.Format = DXGI_FORMAT_R8_UNORM;
        device->CreateShaderResourceView(nullptr, &v, cpu);
        v.Format = DXGI_FORMAT_R8G8_UNORM;
        device->CreateShaderResourceView(nullptr, &v, {cpu.ptr + srvSize});
        v.Format = format;
        device->CreateShaderResourceView(tex, &v, {cpu.ptr + 2 * srvSize});
    }

    void drawPass(ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const D3D12_VIEWPORT& viewport,
                  const D3D12_RECT& scissor, const Params& p, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu) {
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        list->SetPipelineState(pso);
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSig.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
        list->SetGraphicsRootDescriptorTable(1, srvGpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }

    bool draw(const AVFrame& frame) {
        const Plan plan = this->plan(frame);

        WaitForSingleObjectEx(frameLatency, 100, TRUE);
        const UINT bb = swapChain->GetCurrentBackBufferIndex();
        if (frameFence[bb] && fence->GetCompletedValue() < frameFence[bb]) {
            fence->SetEventOnCompletion(frameFence[bb], fenceEvent);
            WaitForSingleObject(fenceEvent, 1000);
        }
        av_frame_free(&inFlight[bb]); // GPU is done with the frame shown in this slot
        collectTimings(bb);

        allocators[bb]->Reset();
        list->Reset(allocators[bb].Get(), nullptr);
        stamp(bb, 0);

        const auto srvCpuBase = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(bb) * kSrvPerFrame * srvSize;
        const auto srvGpuBase = srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(bb) * kSrvPerFrame * srvSize;
        const D3D12_CPU_DESCRIPTOR_HANDLE passACpu{srvCpuBase}, finalCpu{srvCpuBase + kSrvPerPass * srvSize};
        const D3D12_GPU_DESCRIPTOR_HANDLE passAGpu{srvGpuBase}, finalGpu{srvGpuBase + kSrvPerPass * srvSize};

        Params p{};
        bindSource(frame, passACpu, p);

        // Final-pass source: either the decoded frame directly, or the end of the enhancement chain.
        bool hdrOk = plan.hdr;
        D3D12_VIEWPORT finalViewport{plan.vp.x, plan.vp.y, plan.vp.width, plan.vp.height, 0, 1};
        Params finalParams = p;
        D3D12_GPU_DESCRIPTOR_HANDLE finalSrv = passAGpu;

        bool neuralOk = false;
        if (plan.enhanced) {
            // Pass A: decode surface -> RGBA8 at source size.
            const auto toRt = transition(srcRgb.Get(), kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &toRt);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + kBackBuffers * rtvSize};
            const D3D12_VIEWPORT vpA{0, 0, static_cast<float>(srcW), static_cast<float>(srcH), 0, 1};
            const D3D12_RECT scA{0, 0, static_cast<LONG>(srcW), static_cast<LONG>(srcH)};
            drawPass(psoFor(kRgbaFormat), rtv, vpA, scA, p, passAGpu);
            const auto toRead = transition(srcRgb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
            list->ResourceBarrier(1, &toRead);
            stamp(bb, 1);

            ID3D12Resource* current = srcRgb.Get();
            DXGI_FORMAT currentFormat = kRgbaFormat;
            UINT curW = srcW, curH = srcH;

            if (plan.neural) {
                // History is meaningless across a seek, a cut in the timeline or a new size.
                const bool jump = frame.pts == AV_NOPTS_VALUE || lastPts == AV_NOPTS_VALUE || frame.pts <= lastPts;
                if (neural->evaluate(list.Get(), srcRgb.Get(), neuralOut.Get(), srcW, srcH, settings.neuralControls, jump)) {
                    D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(neuralOut.Get()),
                                                   transition(neuralOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                    list->ResourceBarrier(2, b);
                    current = neuralOut.Get();
                    neuralOk = true;
                } else {
                    neuralUnavailable = true;
                    status.flags &= ~EnhanceStatus::NeuralActive;
                    status.flags |= EnhanceStatus::NeuralUnavailable;
                }
            }
            lastPts = frame.pts;
            stamp(bb, 2);

            if (plan.vsr) {
                if (vsr->evaluate(list.Get(), current, srcW, srcH, vsrOut.Get(), vsrW, vsrH, settings.vsrQuality)) {
                    D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(vsrOut.Get()),
                                                   transition(vsrOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                    list->ResourceBarrier(2, b);
                    current = vsrOut.Get();
                    curW = vsrW;
                    curH = vsrH;
                } else {
                    vsrUnavailable = true; // fall back to bilinear from the next frame on
                    status.flags &= ~EnhanceStatus::VsrActive;
                    status.flags |= EnhanceStatus::VsrUnavailable;
                }
            }
            stamp(bb, 3);

            if (plan.hdr) {
                const auto params = hdrParams();
                if (curW == hdrW && curH == hdrH &&
                    trueHdr->evaluate(list.Get(), current, hdrOut.Get(), hdrW, hdrH, params)) {
                    D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(hdrOut.Get()),
                                                   transition(hdrOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                    list->ResourceBarrier(2, b);
                    current = hdrOut.Get();
                    currentFormat = kHdrFormat;
                } else {
                    hdrOk = false;
                    hdrUnavailable = true;
                    status.flags &= ~EnhanceStatus::HdrActive;
                    status.flags |= EnhanceStatus::HdrUnavailable;
                }
            }
            stamp(bb, 4);

            bindRgb(finalCpu, current, currentFormat);
            finalSrv = finalGpu;
            finalParams = Params{};
            finalParams.uvScale[0] = finalParams.uvScale[1] = 1.0f;
            // The swapchain is FP16 linear whenever HDR was planned; an SDR image needs linearising.
            finalParams.mode = (plan.hdr && !hdrOk) ? 2 : 1;
            if (current == vsrOut.Get() || current == hdrOut.Get()) {
                // Already at on-screen size when VSR ran: draw it 1:1 on whole pixels.
                if (curW == plan.outW && curH == plan.outH) {
                    finalViewport.TopLeftX = std::floor(plan.vp.x);
                    finalViewport.TopLeftY = std::floor(plan.vp.y);
                    finalViewport.Width = static_cast<float>(curW);
                    finalViewport.Height = static_cast<float>(curH);
                }
            }
        } else {
            for (UINT i = 1; i <= 4; ++i) stamp(bb, i);
        }

        // Final pass into the back buffer.
        const auto toRt = transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toRt);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + bb * rtvSize};
        const float black[4] = {0, 0, 0, 1};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        drawPass(psoFor(bbFormat), rtv, finalViewport, scissor, finalParams, finalSrv);

        std::vector<D3D12_RESOURCE_BARRIER> after;
        after.push_back(transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        if (plan.vsr && (status.flags & EnhanceStatus::VsrActive))
            after.push_back(transition(vsrOut.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (plan.hdr && hdrOk)
            after.push_back(transition(hdrOut.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (neuralOk) after.push_back(transition(neuralOut.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        stamp(bb, 5);
        if (queryHeap) {
            list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, bb * kStamps, kStamps, queryReadback.Get(),
                                   bb * kStamps * sizeof(UINT64));
            timed[bb] = true;
        }
        list->Close();

        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const HRESULT hr = swapChain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        frameFence[bb] = ++fenceValue;
        queue->Signal(fence.Get(), frameFence[bb]);

        // Keep the decoder surface alive until the GPU has sampled it.
        inFlight[bb] = av_frame_clone(&frame);
        if (last != &frame) {
            av_frame_free(&last);
            last = av_frame_clone(&frame);
        }
        if (FAILED(hr)) {
            log::error("presenter: Present failed 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        return true;
    }

    ngx::TrueHdrParams hdrParams() const {
        auto params = settings.hdr;
        if (params.peakNits == 0) params.peakNits = hdrPeak > 0 ? static_cast<uint32_t>(hdrPeak) : 1000;
        return ngx::TrueHdr::clamp(params);
    }
};

std::unique_ptr<D3D12Presenter> D3D12Presenter::create(gpu::D3D12Context& gpu, ngx::NgxRuntime* ngx,
                                                       telemetry::Telemetry* telemetry) {
    auto impl = std::make_unique<Impl>();
    impl->gpu = &gpu;
    impl->device = gpu.device();
    impl->queue = gpu.directQueue();
    impl->ngx = ngx && ngx->initialized() ? ngx : nullptr;
    if (telemetry) {
        impl->gpuStage = &telemetry->stage("gpu.frame");
        impl->convertStage = &telemetry->stage("gpu.convert");
        impl->vsrStage = &telemetry->stage("gpu.vsr");
        impl->hdrStage = &telemetry->stage("gpu.truehdr");
    }
    if (!impl->device || !impl->queue || !impl->init()) return nullptr;
    return std::unique_ptr<D3D12Presenter>(new D3D12Presenter(std::move(impl)));
}

D3D12Presenter::D3D12Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
D3D12Presenter::~D3D12Presenter() = default;

bool D3D12Presenter::present(const AVFrame& frame) {
    std::lock_guard lock(impl_->mutex);
    return impl_->draw(frame);
}

void D3D12Presenter::redraw() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->last) {
        AVFrame* copy = av_frame_clone(impl_->last);
        impl_->draw(*copy);
        av_frame_free(&copy);
    }
}

void D3D12Presenter::resize(uint32_t width, uint32_t height, float scaleX, float scaleY) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->rebuildSwapchain(width, height, impl_->bbFormat);
        // The panel is sized in DIPs; undo the DPI scale so one swapchain pixel = one screen pixel.
        if (scaleX > 0 && scaleY > 0) {
            DXGI_MATRIX_3X2_F m{1.0f / scaleX, 0, 0, 1.0f / scaleY, 0, 0};
            impl_->swapChain->SetMatrixTransform(&m);
        }
    }
    redraw();
}

IDXGISwapChain1* D3D12Presenter::swapChain() const { return impl_->swapChain.Get(); }

void D3D12Presenter::setEnhance(const EnhanceSettings& settings) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->settings == settings) return;
        // A fresh request gets a fresh attempt (e.g. after a driver update or turning HDR on).
        if (settings.vsr != impl_->settings.vsr) impl_->vsrUnavailable = false;
        if (settings.neural != impl_->settings.neural) impl_->neuralUnavailable = false;
        if (settings.trueHdr != impl_->settings.trueHdr) {
            impl_->hdrUnavailable = false;
            impl_->hdrDisplay = displayHdrInfo(&impl_->hdrPeak);
        }
        impl_->settings = settings;
        impl_->settings.vsrQuality = std::clamp<uint32_t>(settings.vsrQuality, 1, 4);
        if (!settings.vsr) impl_->status.gpuVsrMs = 0;
        if (!settings.neural) impl_->status.gpuNeuralMs = 0;
        if (!settings.trueHdr) impl_->status.gpuHdrMs = 0;
    }
    redraw();
}

EnhanceSettings D3D12Presenter::enhance() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->settings;
}

EnhanceStatus D3D12Presenter::enhanceStatus() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}

double D3D12Presenter::refreshInterval() const {
    // Queried at most once per second: the render loop asks for every frame.
    static thread_local double cached = 1.0 / 60.0;
    static thread_local ULONGLONG lastQuery = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastQuery > 1000) {
        lastQuery = now;
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.rateRefresh.uiNumerator) {
            cached = static_cast<double>(info.rateRefresh.uiDenominator) / info.rateRefresh.uiNumerator;
        }
    }
    return cached;
}

} // namespace bv::render

#else

namespace bv::render {
bool displayHdrInfo(float* peakNits) {
    if (peakNits) *peakNits = 0;
    return false;
}
} // namespace bv::render

#endif // _WIN32
