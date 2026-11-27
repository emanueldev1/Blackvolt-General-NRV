#include "D3D12Presenter.h"

#if defined(_WIN32)

#include "ColorMatrix.h"
#include "core/Log.h"
#include "gpu/D3D12Context.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
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
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// Fullscreen triangle; samples luma + chroma planes and applies a YUV->RGB matrix.
// Mode 1 = packed RGB texture (software fallback), sampled as is.
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

float4 PSMain(VSOut i) : SV_Target {
    if (mode == 1) return float4(rgbTex.Sample(linearSampler, i.uv).rgb, 1);
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

} // namespace

struct D3D12Presenter::Impl {
    gpu::D3D12Context* gpu = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    ComPtr<IDXGISwapChain3> swapChain;
    HANDLE frameLatency = nullptr;
    bool tearing = false;
    UINT width = 64, height = 64;

    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    UINT rtvSize = 0, srvSize = 0;
    std::array<ComPtr<ID3D12Resource>, kBackBuffers> backBuffers;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;

    std::array<ComPtr<ID3D12CommandAllocator>, kBackBuffers> allocators;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    std::array<UINT64, kBackBuffers> frameFence{};
    // Frames the GPU may still be reading; released once their fence completes.
    std::array<AVFrame*, kBackBuffers> inFlight{};

    // Software fallback: frames converted to BGRA and uploaded.
    SwsContext* sws = nullptr;
    ComPtr<ID3D12Resource> rgbTex, upload;
    UINT rgbW = 0, rgbH = 0;

    AVFrame* last = nullptr; // for redraw()
    std::mutex mutex;

    ~Impl() {
        waitIdle();
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

    bool init() {
        ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        BOOL allow = FALSE;
        tearing = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = kBackBufferFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForComposition(queue, &sd, nullptr, &sc1)) || FAILED(sc1.As(&swapChain))) {
            log::error("presenter: CreateSwapChainForComposition failed");
            return false;
        }
        swapChain->SetMaximumFrameLatency(1); // lowest latency: render only when a buffer is free
        frameLatency = swapChain->GetFrameLatencyWaitableObject();

        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap));
        rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        // 3 SRVs (luma, chroma, rgb) per back buffer so in-flight frames keep their descriptors.
        D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kBackBuffers,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap));
        srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (auto& a : allocators) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!createPipeline()) return false;
        createBackBufferViews();
        return true;
    }

    bool createPipeline() {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
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

        auto vs = compile("VSMain", "vs_5_1");
        auto ps = compile("PSMain", "ps_5_1");
        if (!vs || !ps) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kBackBufferFormat;
        pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            log::error("presenter: PSO failed");
            return false;
        }
        return true;
    }

    void createBackBufferViews() {
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i]));
            device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, {h.ptr + i * rtvSize});
        }
    }

    void resizeLocked(UINT w, UINT h) {
        w = std::max<UINT>(w, 8);
        h = std::max<UINT>(h, 8);
        if (w == width && h == height) return;
        waitIdle();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            backBuffers[i].Reset();
            av_frame_free(&inFlight[i]);
            frameFence[i] = 0;
        }
        const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(swapChain->ResizeBuffers(kBackBuffers, w, h, kBackBufferFormat, flags))) {
            log::error("presenter: ResizeBuffers failed");
        }
        width = w;
        height = h;
        createBackBufferViews();
    }

    // Software frames: convert to BGRA on the CPU and upload. Only used when GPU decode is
    // unavailable for the codec, so simplicity wins over speed here.
    bool uploadSoftware(const AVFrame& f, ID3D12GraphicsCommandList* cl) {
        const UINT w = static_cast<UINT>(f.width), h = static_cast<UINT>(f.height);
        if (!rgbTex || rgbW != w || rgbH != h) {
            waitIdle();
            D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
            device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&rgbTex));
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
            D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
            b.Transition = {rgbTex.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
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
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {rgbTex.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        cl->ResourceBarrier(1, &b);
        return true;
    }

    bool draw(const AVFrame& frame) {
        WaitForSingleObjectEx(frameLatency, 100, TRUE);
        const UINT bb = swapChain->GetCurrentBackBufferIndex();
        if (frameFence[bb] && fence->GetCompletedValue() < frameFence[bb]) {
            fence->SetEventOnCompletion(frameFence[bb], fenceEvent);
            WaitForSingleObject(fenceEvent, 1000);
        }
        av_frame_free(&inFlight[bb]); // GPU is done with the frame shown in this slot

        allocators[bb]->Reset();
        list->Reset(allocators[bb].Get(), pso.Get());

        Params p{};
        auto srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<SIZE_T>(bb) * 3 * srvSize;
        auto srvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvGpu.ptr += static_cast<UINT64>(bb) * 3 * srvSize;
        p.uvScale[0] = p.uvScale[1] = 1.0f;

        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
        nullDesc.Format = DXGI_FORMAT_R8_UNORM;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;

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
            device->CreateShaderResourceView(tex, &luma, srvCpu);
            device->CreateShaderResourceView(tex, &chroma, {srvCpu.ptr + srvSize});
            device->CreateShaderResourceView(nullptr, &nullDesc, {srvCpu.ptr + 2 * srvSize});
            // Decoder surfaces are often padded (e.g. 1088 lines); sample only the visible part.
            p.uvScale[0] = static_cast<float>(frame.width) / static_cast<float>(desc.Width);
            p.uvScale[1] = static_cast<float>(frame.height) / static_cast<float>(desc.Height);
            p.matrix = yuvToRgb(matrixFor(frame), frame.color_range == AVCOL_RANGE_JPEG);
            p.mode = 0;
        } else {
            uploadSoftware(frame, list.Get());
            D3D12_SHADER_RESOURCE_VIEW_DESC rgb = nullDesc;
            rgb.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            device->CreateShaderResourceView(nullptr, &nullDesc, srvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC nullChroma = nullDesc;
            nullChroma.Format = DXGI_FORMAT_R8G8_UNORM;
            device->CreateShaderResourceView(nullptr, &nullChroma, {srvCpu.ptr + srvSize});
            device->CreateShaderResourceView(rgbTex.Get(), &rgb, {srvCpu.ptr + 2 * srvSize});
            p.mode = 1;
        }

        D3D12_RESOURCE_BARRIER toRt{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        toRt.Transition = {backBuffers[bb].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
        list->ResourceBarrier(1, &toRt);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + bb * rtvSize};
        const float black[4] = {0, 0, 0, 1};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, black, 0, nullptr);

        const auto vp = fitAspect(static_cast<float>(frame.width), static_cast<float>(frame.height),
                                  frame.sample_aspect_ratio.num ? static_cast<float>(av_q2d(frame.sample_aspect_ratio)) : 1.0f,
                                  static_cast<float>(width), static_cast<float>(height));
        D3D12_VIEWPORT viewport{vp.x, vp.y, vp.width, vp.height, 0, 1};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);

        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSig.Get());
        list->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
        list->SetGraphicsRootDescriptorTable(1, srvGpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toPresent = toRt;
        std::swap(toPresent.Transition.StateBefore, toPresent.Transition.StateAfter);
        list->ResourceBarrier(1, &toPresent);
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
};

std::unique_ptr<D3D12Presenter> D3D12Presenter::create(gpu::D3D12Context& gpu) {
    auto impl = std::make_unique<Impl>();
    impl->gpu = &gpu;
    impl->device = gpu.device();
    impl->queue = gpu.directQueue();
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
        impl_->resizeLocked(width, height);
        // The panel is sized in DIPs; undo the DPI scale so one swapchain pixel = one screen pixel.
        if (scaleX > 0 && scaleY > 0) {
            DXGI_MATRIX_3X2_F m{1.0f / scaleX, 0, 0, 1.0f / scaleY, 0, 0};
            impl_->swapChain->SetMatrixTransform(&m);
        }
    }
    redraw();
}

IDXGISwapChain1* D3D12Presenter::swapChain() const { return impl_->swapChain.Get(); }

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

#endif // _WIN32
