#include "D3D12Presenter.h"

#if defined(_WIN32)

#include "ColorMatrix.h"
#include "core/Log.h"
#include "gpu/D3D12Context.h"
#include "ngx/DlssNrSession.h"
#include "ngx/NgxRuntime.h"
#include "ngx/VideoSuperResolution.h"
#include "telemetry/Telemetry.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/mastering_display_metadata.h>
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
constexpr UINT kSrvPerFrame = 3 * kSrvPerPass; // pass A (decode -> RGB), final pass, A/B original
// start, converted, NR prepared, NR evaluated, NR composited, interpolated, vsr, hdr, end
constexpr UINT kStamps = 9;
constexpr double kSmoothing = 0.1;

// Fullscreen triangle.
// mode 0: YUV planes -> RGB with a matrix.  mode 1: RGB texture as is.
// mode 2: sRGB texture -> linear scRGB (an SDR frame shown on the FP16 swapchain).
constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
    float4 row0; float4 row1; float4 row2; float4 uvScale;
    uint mode; uint transfer; uint hdrOut; uint pad0;   // transfer: 0 SDR, 1 PQ (HDR10), 2 HLG
    float sdrWhite; float contentPeak; float2 pad1;     // nits
};
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

float3 linearToSrgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// SMPTE ST 2084 (PQ) EOTF: code value -> nits.
float3 pqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

// ARIB STD-B67 (HLG): inverse OETF, then the OOTF for a 1000-nit display (system gamma 1.2).
float3 hlgToNits(float3 e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = saturate(e);
    const float3 scene = e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
    const float y = dot(scene, float3(0.2627, 0.6780, 0.0593));
    return 1000.0 * scene * pow(max(y, 1e-6), 0.2);
}

static const float3x3 kBt2020To709 = {
     1.6605, -0.5876, -0.0728,
    -0.1246,  1.1329, -0.0083,
    -0.0182, -0.1006,  1.1187,
};

// HDR -> SDR: luminance-based extended Reinhard up to the content's real peak (keeps hue), then a
// soft roll-off of channels pushed above white so saturated highlights do not clip harshly.
float3 toneMap(float3 nits) {
    float3 l = nits / sdrWhite;
    const float w = max(contentPeak / sdrWhite, 1.0);
    const float y = dot(l, float3(0.2126, 0.7152, 0.0722));
    const float yt = y * (1.0 + y / (w * w)) / (1.0 + y);
    if (y > 0) l *= yt / y;
    const float m = max(l.r, max(l.g, l.b));
    if (m > 1.0) l = lerp(l / m, float3(1, 1, 1), saturate((m - 1.0) * 0.5));
    return l;
}

float4 PSMain(VSOut i) : SV_Target {
    if (mode == 1) return float4(rgbTex.Sample(linearSampler, i.uv).rgb, 1);
    if (mode == 2) return float4(srgbToLinear(saturate(rgbTex.Sample(linearSampler, i.uv).rgb)), 1);
    float4 yuv = float4(lumaTex.Sample(linearSampler, i.uv), chromaTex.Sample(linearSampler, i.uv), 1);
    const float3 rgb = float3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv));
    if (transfer != 0) {
        const float3 nits = transfer == 1 ? pqToNits(rgb) : hlgToNits(rgb);
        const float3 lin = max(mul(kBt2020To709, nits), 0.0);
        if (hdrOut != 0) return float4(lin / 80.0, 1);   // scRGB: 1.0 = 80 nits
        return float4(linearToSrgb(toneMap(lin)), 1);
    }
    return float4(saturate(rgb), 1);
}
)";

struct alignas(16) Params {
    std::array<float, 12> matrix;
    float uvScale[4];
    uint32_t mode;
    uint32_t transfer; // 0 SDR, 1 PQ, 2 HLG
    uint32_t hdrOut;   // 1 = write scRGB (HDR passthrough), 0 = tone map to SDR
    uint32_t pad0;
    float sdrWhite;
    float contentPeak;
    float pad1[2];
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

// PNG through WIC (32-bit BGRA, alpha ignored). UTF-8 path.
bool savePng(const std::string& path, UINT w, UINT h, const uint8_t* bgra) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wide(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
             SUCCEEDED(factory->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             format == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra))) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) log::warn("screenshot: could not write {}", path);
    return ok;
}

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
    telemetry::StageStats* nrStage = nullptr;
    telemetry::StageStats* nrCompositeStage = nullptr;
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
    // DLSS NR must be evaluated on a command list that has recorded nothing before it, so a frame
    // with NR is split: "list" up to the NR input, then "nrList" for everything after.
    std::array<ComPtr<ID3D12CommandAllocator>, kBackBuffers> nrAllocators;
    ComPtr<ID3D12GraphicsCommandList> nrList;
    ID3D12GraphicsCommandList* cmd = nullptr; // the list being recorded
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
    // Frame interpolation: slot curSlot holds the last real frame shown, 1-curSlot the staged
    // next one (already converted and through NR) while generated frames are shown.
    std::unique_ptr<enhance::FrameInterpolation> interp;
    std::unique_ptr<enhance::Denoise> denoiser;
    std::unique_ptr<enhance::NisScaler> nis;
    std::unique_ptr<enhance::PostFx> postfx;
    bool postfxWasOn = false;
    bool denoiseWasOn = false;
    uint32_t curSlot = 0;
    bool curSlotValid = false, stagedValid = false;
    int64_t stagedPts = AV_NOPTS_VALUE;
    // Screenshot request, served by the next draw.
    std::string screenshotPath;
    bool screenshotDone = false, screenshotOk = false;
    ComPtr<ID3D12Resource> shotReadback;
    enhance::NrLocator nrLocator;
    std::unique_ptr<enhance::NeuralRendering> neural;
    std::shared_ptr<ngx::DlssNrSession> nrSession; // keeps the NR runtime loaded between features
    struct NrBuilt {
        std::unique_ptr<enhance::NeuralRendering> feature;
        enhance::NeuralControls controls{};
        UINT w = 0, h = 0;
        std::string note;
    };
    std::future<NrBuilt> nrBuild;
    ComPtr<ID3D12CommandAllocator> nrSetupAllocator;
    ComPtr<ID3D12GraphicsCommandList> nrSetupList;
    ComPtr<ID3D12Fence> nrSetupFence;
    UINT64 nrSetupValue = 0;
    HANDLE nrSetupEvent = nullptr;

    // Builds an NR feature for the current settings on a worker thread, with its own command list
    // and fence (command queues are free-threaded).
    void startNrBuild(UINT w, UINT h) {
        if (!nrSetupAllocator) {
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nrSetupAllocator));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nrSetupAllocator.Get(), nullptr,
                                      IID_PPV_ARGS(&nrSetupList));
            nrSetupList->Close();
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nrSetupFence));
            nrSetupEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        const auto controls = settings.neuralControls;
        // A different runtime cannot share the loaded one: let it go first.
        if (neural && neural->variant() != controls.variant && controls.variant != ngx::NrVariant::Auto) {
            waitIdle();
            neural.reset();
            nrSession.reset();
        }
        auto keep = nrSession; // the worker must not be the one unloading the runtime under us
        nrBuild = std::async(std::launch::async, [this, controls, w, h, keep]() mutable {
            NrBuilt out;
            out.controls = controls;
            out.w = w;
            out.h = h;
            auto run = [&] {
                nrSetupAllocator->Reset();
                nrSetupList->Reset(nrSetupAllocator.Get(), nullptr);
                auto f = enhance::NeuralRendering::create(ngx, device, nrSetupList.Get(), nrLocator, w, h, controls, &out.note);
                nrSetupList->Close();
                ID3D12CommandList* lists[] = {nrSetupList.Get()};
                queue->ExecuteCommandLists(1, lists);
                const UINT64 v = ++nrSetupValue;
                queue->Signal(nrSetupFence.Get(), v);
                if (nrSetupFence->GetCompletedValue() < v) {
                    nrSetupFence->SetEventOnCompletion(v, nrSetupEvent);
                    WaitForSingleObject(nrSetupEvent, 5000);
                }
                return f;
            };
            out.feature = run();
            if (!out.feature && keep) {
                // Another runtime is still loaded (e.g. Auto picked a different one): drop ours and retry.
                keep.reset();
                out.note.clear();
                out.feature = run();
            }
            return out;
        });
    }
    enhance::NeuralControls neuralBuilt{}; // controls the feature was created with
    UINT neuralW = 0, neuralH = 0;
    bool neuralUnavailable = false;
    bool neuralMissingRuntime = false; // failed only because no nvngx_dlssnr.dll was installed
    ULONGLONG neuralRetryTick = 0;
    std::chrono::steady_clock::time_point lastNrEval{}; // anti-flicker step
    std::string neuralNote;
    int64_t lastPts = AV_NOPTS_VALUE;

    AVFrame* last = nullptr; // for redraw()
    mutable std::mutex mutex;

    // ---- shared with the UI thread without waiting for `mutex` (held for a whole frame) ----
    mutable std::mutex shared;
    std::optional<EnhanceSettings> pending; // set by setEnhance, applied by the next draw
    EnhanceSettings published;              // what the render path runs with
    EnhanceStatus statusCopy;               // snapshot published after each frame
    std::atomic<uint32_t> factor{1};        // interpolation factor, read by the player every frame
    std::atomic<ULONGLONG> lastDrawTick{0};

    // Redraws the last frame off the caller's thread when nothing is playing (paused, menu, still
    // stream), so changing a setting shows at once without blocking the UI.
    std::thread worker;
    std::condition_variable workerCv;
    std::mutex workerMutex;
    bool redrawWanted = false, forceRedraw = false, workerQuit = false;

    void startWorker() {
        worker = std::thread([this] {
            std::unique_lock lock(workerMutex);
            while (true) {
                workerCv.wait(lock, [&] { return workerQuit || redrawWanted; });
                if (workerQuit) return;
                const bool force = forceRedraw;
                redrawWanted = forceRedraw = false;
                lock.unlock();
                // Live video applies the change on its own next frame; only a still picture needs this.
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                if (force || GetTickCount64() - lastDrawTick.load() > 150) {
                    std::lock_guard drawLock(mutex);
                    redrawLast();
                }
                lock.lock();
            }
        });
    }

    void stopWorker() {
        {
            std::lock_guard lock(workerMutex);
            workerQuit = true;
        }
        workerCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void requestRedraw(bool force = false) {
        {
            std::lock_guard lock(workerMutex);
            redrawWanted = true;
            forceRedraw = forceRedraw || force;
        }
        workerCv.notify_one();
    }

    // With `mutex` held.
    void redrawLast() {
        if (!last) {
            takePending();
            return;
        }
        AVFrame* copy = av_frame_clone(last);
        draw(*copy);
        av_frame_free(&copy);
    }

    void publishStatus() {
        std::lock_guard lock(shared);
        statusCopy = status;
    }

    // With `mutex` held: applies settings handed over by setEnhance().
    void takePending() {
        std::optional<EnhanceSettings> next;
        {
            std::lock_guard lock(shared);
            next.swap(pending);
        }
        if (!next || *next == settings) return;
        const EnhanceSettings& in = *next;
        // A fresh request gets a fresh attempt (e.g. after a driver update or turning HDR on).
        if (in.vsr != settings.vsr) vsrUnavailable = false;
        // Turning NR on again, or choosing another runtime / model / size, gets a fresh attempt.
        if (in.neural != settings.neural || !in.neuralControls.sameFeature(settings.neuralControls)) neuralUnavailable = false;
        if (in.trueHdr != settings.trueHdr) {
            hdrUnavailable = false;
            hdrDisplay = displayHdrInfo(&hdrPeak);
        }
        settings = in;
        settings.interpolation = std::clamp<uint32_t>(in.interpolation, 1, 4);
        settings.denoise = std::min<uint32_t>(in.denoise, 3);
        settings.sharpness = std::clamp(in.sharpness, 0.0f, 1.0f);
        settings.vsrQuality = std::clamp<uint32_t>(in.vsrQuality, 1, 4);
        if (!settings.vsr) status.gpuVsrMs = 0;
        if (!settings.neural) {
            status.gpuNeuralMs = 0;
            status.gpuNrPrepareMs = 0;
            status.gpuNrCompositeMs = 0;
        }
        if (!settings.trueHdr) status.gpuHdrMs = 0;
        std::lock_guard lock(shared);
        published = settings;
        statusCopy = status;
    }

    ~Impl() {
        if (nrBuild.valid()) nrBuild.wait();
        nrBuild = {};
        waitIdle();
        neural.reset();
        nrSession.reset();
        if (nrSetupEvent) CloseHandle(nrSetupEvent);
        interp.reset();
        denoiser.reset();
        nis.reset();
        postfx.reset();
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
        for (auto& a : nrAllocators) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nrAllocators[0].Get(), nullptr, IID_PPV_ARGS(&nrList));
        nrList->Close();
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
        bool enhanced = false, neural = false, vsr = false, nis = false, hdr = false, interp = false, denoise = false,
             postfx = false;
        uint32_t transfer = 0;    // source: 0 SDR, 1 PQ, 2 HLG
        bool passthrough = false; // HDR source shown as HDR (scRGB swapchain)
        float contentPeak = 1000; // nits, from the stream's metadata when present
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

        // HDR sources (hardware frames): PQ / HLG, and their real peak for tone mapping.
        if (frame.format == AV_PIX_FMT_D3D12) {
            if (frame.color_trc == AVCOL_TRC_SMPTE2084) p.transfer = 1;
            else if (frame.color_trc == AVCOL_TRC_ARIB_STD_B67) p.transfer = 2;
        }
        if (p.transfer) {
            if (const auto* sd = av_frame_get_side_data(&frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
                const auto* cll = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
                if (cll->MaxCLL > 100) p.contentPeak = static_cast<float>(cll->MaxCLL);
            } else if (const auto* md = av_frame_get_side_data(&frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
                const auto* mdm = reinterpret_cast<const AVMasteringDisplayMetadata*>(md->data);
                if (mdm->has_luminance && mdm->max_luminance.den)
                    p.contentPeak = std::max(100.0f, static_cast<float>(av_q2d(mdm->max_luminance)));
            }
            p.contentPeak = std::clamp(p.contentPeak, 203.0f, 10000.0f);
        }
        if (settings.trueHdr || p.transfer) {
            if (factory && !factory->IsCurrent()) { // display configuration changed
                factory.Reset();
                CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
                hdrDisplay = displayHdrInfo(&hdrPeak);
            }
            status.displayPeakNits = hdrPeak;
            if (!hdrDisplay) status.flags |= EnhanceStatus::DisplaySdr;
        }

        // DLSS NR first in the chain, on the decoded picture (internally at its own size).
        // A runtime downloaded while playing is picked up within a second.
        if (settings.neural && neuralUnavailable && neuralMissingRuntime && nrLocator && GetTickCount64() - neuralRetryTick > 1000) {
            neuralRetryTick = GetTickCount64();
            const auto wanted = settings.neuralControls.variant;
            const bool present = wanted == ngx::NrVariant::Auto
                                     ? !nrLocator(ngx::NrVariant::Compat).empty() || !nrLocator(ngx::NrVariant::Original).empty()
                                     : !nrLocator(wanted).empty();
            if (present) neuralUnavailable = neuralMissingRuntime = false;
        }
        p.neural = settings.neural && !neuralUnavailable;
        // NR features take 1-2 s to build. They are built on a worker thread while the video keeps
        // playing (without NR, or with the previous feature until the swap) instead of freezing it.
        if (nrBuild.valid() && nrBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto built = nrBuild.get();
            if (built.feature && built.controls.sameFeature(settings.neuralControls) && built.w == inW && built.h == inH) {
                waitIdle();
                neural = std::move(built.feature);
                nrSession = neural->session();
                neuralBuilt = built.controls;
                neuralW = built.w;
                neuralH = built.h;
                lastPts = AV_NOPTS_VALUE; // fresh history
            } else if (!built.feature && built.controls.sameFeature(settings.neuralControls)) {
                neuralNote = built.note;
                neuralUnavailable = true;
                p.neural = false;
                const auto wanted = settings.neuralControls.variant;
                neuralMissingRuntime = nrLocator && (wanted == ngx::NrVariant::Auto
                                                         ? nrLocator(ngx::NrVariant::Compat).empty() &&
                                                               nrLocator(ngx::NrVariant::Original).empty()
                                                         : nrLocator(wanted).empty());
                log::info("DLSS NR not available: {}", neuralNote);
            }
            // A build for settings that changed meanwhile is dropped; the check below starts the right one.
        }
        const bool nrCurrent = neural && neuralW == inW && neuralH == inH && neuralBuilt.sameFeature(settings.neuralControls);
        if (p.neural && !nrCurrent && !nrBuild.valid()) startNrBuild(inW, inH);
        if (p.neural && !nrCurrent) {
            // Still building: no NR meanwhile (one thread at a time inside nvngx_dlssnr.dll).
            p.neural = false;
            status.neuralNote = "Preparando DLSS NR…";
        }
        if (!settings.neural && neural) {
            waitIdle();
            neural.reset(); // free the VRAM and the NR session when the user turns it off
        }
        if (settings.neural && neuralUnavailable) status.flags |= EnhanceStatus::NeuralUnavailable;
        if (!(settings.neural && nrBuild.valid())) status.neuralNote = settings.neural && neuralUnavailable ? neuralNote : std::string();
        if (p.neural && neural) {
            status.nrWidth = neural->nrWidth();
            status.nrHeight = neural->nrHeight();
            status.nrVariant = neural->variant();
            status.nrLabel = neural->label();
        } else {
            status.nrWidth = status.nrHeight = 0;
            status.nrVariant = ngx::NrVariant::Auto;
            status.nrLabel.clear();
        }

        // NVIDIA Image Scaling: chosen, or the fallback when RTX VSR cannot run (no NGX, AMD, Intel).
        const bool wantNis = settings.vsr && (settings.scaler == 1 || !ngx || vsrUnavailable);
        p.vsr = settings.vsr && ngx && !vsrUnavailable && !wantNis;
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
        if (wantNis) {
            if (p.outW < inW || p.outH < inH) {
                status.flags |= EnhanceStatus::VsrBypassed;
            } else {
                uint32_t nisW = 0, nisH = 0;
                enhance::NisScaler::outputSize(inW, inH, p.outW, p.outH, nisW, nisH);
                if (!nis || nis->inWidth() != inW || nis->inHeight() != inH || nis->outWidth() != nisW ||
                    nis->outHeight() != nisH) {
                    waitIdle();
                    nis.reset();
                    nis = runSetup([&](ID3D12GraphicsCommandList* cl) {
                        return enhance::NisScaler::create(device, cl, inW, inH, nisW, nisH);
                    });
                }
                p.nis = nis != nullptr;
            }
        } else if (nis) {
            waitIdle();
            nis.reset();
        }
        if (settings.vsr && vsrUnavailable && !p.nis) status.flags |= EnhanceStatus::VsrUnavailable;
        if (settings.trueHdr && hdrUnavailable) status.flags |= EnhanceStatus::HdrUnavailable;

        // Interpolation and A/B need the picture as RGBA, so they force the enhancement chain.
        p.interp = settings.interpolation > 1;
        if (p.interp && (!interp || interp->width() != inW || interp->height() != inH)) {
            waitIdle();
            interp = enhance::FrameInterpolation::create(device, inW, inH);
            curSlotValid = stagedValid = false;
        }
        if (!p.interp && interp) {
            waitIdle();
            interp.reset();
            curSlotValid = stagedValid = false;
        }
        p.interp = p.interp && interp;
        // Denoise: before NR, at source size. Switching it changes NR's input, so wait for the GPU.
        p.denoise = settings.denoise > 0;
        if (p.denoise != denoiseWasOn) {
            waitIdle();
            denoiseWasOn = p.denoise;
        }
        if (p.denoise && (!denoiser || denoiser->width() != inW || denoiser->height() != inH)) {
            waitIdle();
            denoiser = enhance::Denoise::create(device, inW, inH);
        }
        if (!p.denoise && denoiser) denoiser.reset();
        p.denoise = p.denoise && denoiser;
        // Colour / FXAA after NR, at source size. Toggling it changes later stages' input.
        p.postfx = !settings.color.identity();
        if (p.postfx != postfxWasOn) {
            waitIdle();
            postfxWasOn = p.postfx;
        }
        if (p.postfx && (!postfx || postfx->width() != inW || postfx->height() != inH)) {
            waitIdle();
            postfx = enhance::PostFx::create(device, inW, inH);
        }
        p.postfx = p.postfx && postfx;
        p.enhanced = p.neural || p.vsr || p.nis || p.hdr || p.interp || p.denoise || p.postfx || settings.compare;
        if (p.enhanced) {
            if (!ensureTexture(srcRgb, srcW, srcH, inW, inH, kRgbaFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kShaderRead)) {
                p.enhanced = p.neural = p.vsr = p.hdr = p.interp = false;
            } else {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + kBackBuffers * rtvSize};
                device->CreateRenderTargetView(srcRgb.Get(), nullptr, rtv);
            }
        }
        if (p.vsr && !ensureTexture(vsrOut, vsrW, vsrH, p.outW, p.outH, kRgbaFormat,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            p.vsr = false;
        }
        const UINT hdrInW = p.vsr ? p.outW : p.nis ? nis->outWidth() : inW;
        const UINT hdrInH = p.vsr ? p.outH : p.nis ? nis->outHeight() : inH;
        if (p.hdr && !ensureTexture(hdrOut, hdrW, hdrH, hdrInW, hdrInH, kHdrFormat,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            p.hdr = false;
        }
        p.enhanced = p.neural || p.vsr || p.nis || p.hdr || p.interp || p.denoise || p.postfx || (settings.compare && srcRgb);

        // HDR source on an HDR display, with no SDR-only stage in the way: show it as HDR.
        p.passthrough = p.transfer && hdrDisplay && settings.hdrPassthrough && !p.enhanced;
        if (p.transfer) status.flags |= EnhanceStatus::HdrSource;
        if (p.passthrough) status.flags |= EnhanceStatus::HdrPassthrough;
        rebuildSwapchain(width, height, (p.hdr || p.passthrough) ? kHdrFormat : kSdrFormat);
        if (p.neural) status.flags |= EnhanceStatus::NeuralActive;
        if (p.vsr) status.flags |= EnhanceStatus::VsrActive;
        if (p.hdr) status.flags |= EnhanceStatus::HdrActive;
        if (p.interp) status.flags |= EnhanceStatus::InterpolationActive;
        if (p.nis) status.flags |= EnhanceStatus::NisActive;
        status.outputWidth = p.enhanced ? (p.vsr ? p.outW : p.nis ? nis->outWidth() : inW) : p.outW;
        status.outputHeight = p.enhanced ? (p.vsr ? p.outH : p.nis ? nis->outHeight() : inH) : p.outH;
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
        if (queryHeap) cmd->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, bb * kStamps + index);
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
        const double convert = ms(0, 1), prepare = ms(1, 2), neuralMs = ms(2, 3), composite = ms(3, 4),
                     interpMs = ms(4, 5), vsrMs = ms(5, 6), hdrMs = ms(6, 7), presentMs = ms(7, 8), total = ms(0, 8);
        const D3D12_RANGE none{0, 0};
        queryReadback->Unmap(0, &none);
        smooth(status.gpuConvertMs, convert);
        smooth(status.gpuNrPrepareMs, prepare);
        smooth(status.gpuNeuralMs, neuralMs);
        smooth(status.gpuNrCompositeMs, composite);
        smooth(status.gpuVsrMs, vsrMs);
        smooth(status.gpuHdrMs, hdrMs);
        smooth(status.gpuPresentMs, presentMs);
        if (interpMs > 0) smooth(status.gpuInterpMs, interpMs);
        smooth(status.gpuTotalMs, total);
        if (gpuStage) gpuStage->record(total * 1000.0);
        if (convertStage) convertStage->record(convert * 1000.0);
        if (nrStage && neuralMs > 0) nrStage->record((prepare + neuralMs) * 1000.0);
        if (nrCompositeStage && composite > 0) nrCompositeStage->record(composite * 1000.0);
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
            uploadSoftware(frame, cmd);
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
        cmd->SetPipelineState(pso);
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(rootSig.Get());
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
        cmd->SetGraphicsRootDescriptorTable(1, srvGpu);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // interpT < 0: show `frame`. 0 < interpT < 1: show a frame generated between the last real
    // frame and `frame` (which is staged, not shown).
    bool draw(const AVFrame& frame, float interpT = -1.0f) {
        takePending();
        lastDrawTick = GetTickCount64();
        const Plan plan = this->plan(frame);
        if (interpT >= 0 && (!plan.interp || !curSlotValid)) return false;

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
        cmd = list.Get();
        stamp(bb, 0);

        const auto srvCpuBase = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(bb) * kSrvPerFrame * srvSize;
        const auto srvGpuBase = srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(bb) * kSrvPerFrame * srvSize;
        const D3D12_CPU_DESCRIPTOR_HANDLE passACpu{srvCpuBase}, finalCpu{srvCpuBase + kSrvPerPass * srvSize},
            compareCpu{srvCpuBase + 2 * kSrvPerPass * srvSize};
        const D3D12_GPU_DESCRIPTOR_HANDLE passAGpu{srvGpuBase}, finalGpu{srvGpuBase + kSrvPerPass * srvSize},
            compareGpu{srvGpuBase + 2 * kSrvPerPass * srvSize};

        Params p{};
        bindSource(frame, passACpu, p);
        p.transfer = plan.transfer;
        p.hdrOut = plan.passthrough ? 1u : 0u;
        p.sdrWhite = 203.0f;
        p.contentPeak = plan.contentPeak;

        // Final-pass source: either the decoded frame directly, or the end of the enhancement chain.
        bool hdrOk = plan.hdr;
        D3D12_VIEWPORT finalViewport{plan.vp.x, plan.vp.y, plan.vp.width, plan.vp.height, 0, 1};
        Params finalParams = p;
        D3D12_GPU_DESCRIPTOR_HANDLE finalSrv = passAGpu;

        bool neuralOk = false, interpOut = false;
        if (plan.enhanced) {
          ID3D12Resource* current = srcRgb.Get();
          // A frame staged for interpolation was already converted and enhanced.
          const bool staged = plan.interp && stagedValid && frame.pts == stagedPts;
          if (staged) {
            current = interp->slot(1 - curSlot);
            for (UINT i = 1; i <= 4; ++i) stamp(bb, i);
          } else {
            // Pass A: decode surface -> RGBA8 at source size.
            const auto toRt = transition(srcRgb.Get(), kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &toRt);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + kBackBuffers * rtvSize};
            const D3D12_VIEWPORT vpA{0, 0, static_cast<float>(srcW), static_cast<float>(srcH), 0, 1};
            const D3D12_RECT scA{0, 0, static_cast<LONG>(srcW), static_cast<LONG>(srcH)};
            drawPass(psoFor(kRgbaFormat), rtv, vpA, scA, p, passAGpu);
            const auto toRead = transition(srcRgb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
            cmd->ResourceBarrier(1, &toRead);
            // Denoise (its cost is reported with the conversion).
            if (plan.denoise) {
                denoiser->run(cmd, srcRgb.Get(), 1.0f, 1.0f, static_cast<float>(settings.denoise));
                current = denoiser->output();
            }
            stamp(bb, 1);
            ID3D12Resource* const nrBase = current;

            if (plan.neural) {
                // NR input (downsampled when NR runs below source size), then submit: the NR
                // evaluation must start a command list of its own.
                neural->prepare(cmd, nrBase);
                stamp(bb, 2);
                cmd->Close();
                ID3D12CommandList* first[] = {cmd};
                queue->ExecuteCommandLists(1, first);
                nrAllocators[bb]->Reset();
                nrList->Reset(nrAllocators[bb].Get(), nullptr);
                cmd = nrList.Get();

                // History is meaningless across a seek, a cut in the timeline or a new size.
                const bool jump = frame.pts == AV_NOPTS_VALUE || lastPts == AV_NOPTS_VALUE || frame.pts <= lastPts;
                const auto now = std::chrono::steady_clock::now();
                const double stepMs = lastNrEval.time_since_epoch().count()
                                          ? std::chrono::duration<double, std::milli>(now - lastNrEval).count()
                                          : 0.0;
                lastNrEval = now;
                if (neural->evaluate(cmd, settings.neuralControls, jump, stepMs)) {
                    stamp(bb, 3);
                    neural->composite(cmd, nrBase, settings.neuralControls);
                    current = neural->output();
                    neuralOk = true;
                } else {
                    neuralUnavailable = true;
                    neuralNote = "DLSS NR falló al procesar el fotograma; se desactivó para este video.";
                    status.flags &= ~EnhanceStatus::NeuralActive;
                    status.flags |= EnhanceStatus::NeuralUnavailable;
                    status.neuralNote = neuralNote;
                    stamp(bb, 3);
                }
            } else {
                stamp(bb, 2);
                stamp(bb, 3);
            }
            lastPts = frame.pts;
            if (plan.postfx) {
                postfx->run(cmd, current, settings.color);
                current = postfx->output();
            }
            stamp(bb, 4);
            if (plan.interp) interp->ingest(cmd, 1 - curSlot, current);
          }
          DXGI_FORMAT currentFormat = kRgbaFormat;
          UINT curW = srcW, curH = srcH;

          // Frame interpolation (generated frame), or bookkeeping for a real one.
          if (plan.interp) {
              if (interpT >= 0) {
                  interp->interpolate(cmd, curSlot, 1 - curSlot, interpT);
                  current = interp->output();
                  interpOut = true;
                  stagedValid = true;
                  stagedPts = frame.pts;
                  ++status.interpolatedFrames;
              } else {
                  curSlot = 1 - curSlot;
                  curSlotValid = true;
                  stagedValid = false;
              }
          }
          stamp(bb, 5);

            if (plan.vsr) {
                if (vsr->evaluate(cmd, current, srcW, srcH, vsrOut.Get(), vsrW, vsrH, settings.vsrQuality)) {
                    D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(vsrOut.Get()),
                                                   transition(vsrOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                    cmd->ResourceBarrier(2, b);
                    current = vsrOut.Get();
                    curW = vsrW;
                    curH = vsrH;
                } else {
                    vsrUnavailable = true; // NIS takes over from the next frame on
                    status.flags &= ~EnhanceStatus::VsrActive;
                    status.flags |= EnhanceStatus::VsrUnavailable;
                }
            } else if (plan.nis) {
                nis->run(cmd, current, settings.sharpness);
                current = nis->output();
                curW = nis->outWidth();
                curH = nis->outHeight();
            }
            stamp(bb, 6);

            if (plan.hdr) {
                const auto params = hdrParams();
                if (curW == hdrW && curH == hdrH &&
                    trueHdr->evaluate(cmd, current, hdrOut.Get(), hdrW, hdrH, params)) {
                    D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(hdrOut.Get()),
                                                   transition(hdrOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                    cmd->ResourceBarrier(2, b);
                    current = hdrOut.Get();
                    currentFormat = kHdrFormat;
                } else {
                    hdrOk = false;
                    hdrUnavailable = true;
                    status.flags &= ~EnhanceStatus::HdrActive;
                    status.flags |= EnhanceStatus::HdrUnavailable;
                }
            }
            stamp(bb, 7);

            bindRgb(finalCpu, current, currentFormat);
            finalSrv = finalGpu;
            finalParams = Params{};
            finalParams.uvScale[0] = finalParams.uvScale[1] = 1.0f;
            // The swapchain is FP16 linear whenever HDR was planned; an SDR image needs linearising.
            finalParams.mode = (plan.hdr && !hdrOk) ? 2 : 1;
            if (current == vsrOut.Get() || current == hdrOut.Get() || (nis && current == nis->output())) {
                // Already at on-screen size when VSR ran: draw it 1:1 on whole pixels.
                if (curW == plan.outW && curH == plan.outH) {
                    finalViewport.TopLeftX = std::floor(plan.vp.x);
                    finalViewport.TopLeftY = std::floor(plan.vp.y);
                    finalViewport.Width = static_cast<float>(curW);
                    finalViewport.Height = static_cast<float>(curH);
                }
            }
        } else {
            for (UINT i = 1; i <= 7; ++i) stamp(bb, i);
        }

        // Final pass into the back buffer.
        const auto toRt = transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &toRt);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + bb * rtvSize};
        const float black[4] = {0, 0, 0, 1};
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        drawPass(psoFor(bbFormat), rtv, finalViewport, scissor, finalParams, finalSrv);

        // A/B: the original picture over the left half of the video area (not on generated frames),
        // or over all of it while the user holds "show original".
        if (settings.compare && plan.enhanced && (interpT < 0 || settings.compare == 2) && srcRgb) {
            bindRgb(compareCpu, srcRgb.Get(), kRgbaFormat);
            Params cp{};
            cp.uvScale[0] = cp.uvScale[1] = 1.0f;
            cp.mode = bbFormat == kHdrFormat ? 2 : 1;
            const D3D12_VIEWPORT vp{plan.vp.x, plan.vp.y, plan.vp.width, plan.vp.height, 0, 1};
            const LONG mid = static_cast<LONG>(plan.vp.x + plan.vp.width / 2);
            const D3D12_RECT left{static_cast<LONG>(plan.vp.x), 0, settings.compare == 2 ? static_cast<LONG>(width) : mid - 1,
                                  static_cast<LONG>(height)};
            drawPass(psoFor(bbFormat), rtv, vp, left, cp, compareGpu);
        }

        // Screenshot: copy what is on screen (video area only) for the requester.
        const bool shot = !screenshotPath.empty() && !screenshotDone;
        D3D12_BOX shotBox{};
        UINT64 shotPitch = 0;
        if (shot) {
            shotBox = {static_cast<UINT>(std::max(0.0f, std::floor(plan.vp.x))), static_cast<UINT>(std::max(0.0f, std::floor(plan.vp.y))), 0,
                       std::min<UINT>(width, static_cast<UINT>(std::ceil(plan.vp.x + plan.vp.width))),
                       std::min<UINT>(height, static_cast<UINT>(std::ceil(plan.vp.y + plan.vp.height))), 1};
            const UINT bytes = bbFormat == kHdrFormat ? 8 : 4;
            shotPitch = ((shotBox.right - shotBox.left) * bytes + 255) & ~255ull;
            const UINT64 need = shotPitch * (shotBox.bottom - shotBox.top);
            if (!shotReadback || shotReadback->GetDesc().Width < need) {
                D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
                D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, need, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
                shotReadback.Reset();
                device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&shotReadback));
            }
            if (shotReadback && shotBox.right > shotBox.left && shotBox.bottom > shotBox.top) {
                const auto toCopy = transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->ResourceBarrier(1, &toCopy);
                D3D12_TEXTURE_COPY_LOCATION dst{shotReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint.Offset = 0;
                dst.PlacedFootprint.Footprint = {bbFormat, shotBox.right - shotBox.left, shotBox.bottom - shotBox.top, 1,
                                                 static_cast<UINT>(shotPitch)};
                D3D12_TEXTURE_COPY_LOCATION src{backBuffers[bb].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = 0;
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &shotBox);
                const auto back = transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmd->ResourceBarrier(1, &back);
            }
        }

        std::vector<D3D12_RESOURCE_BARRIER> after;
        after.push_back(transition(backBuffers[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        if (plan.vsr && (status.flags & EnhanceStatus::VsrActive))
            after.push_back(transition(vsrOut.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (plan.hdr && hdrOk)
            after.push_back(transition(hdrOut.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        cmd->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        if (neuralOk) neural->finish(cmd);
        if (interpOut) interp->finish(cmd);
        if (plan.denoise && denoiser) denoiser->finish(cmd);
        if (plan.nis && nis) nis->finish(cmd);
        if (plan.postfx && postfx) postfx->finish(cmd);
        stamp(bb, 8);
        if (queryHeap) {
            cmd->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, bb * kStamps, kStamps, queryReadback.Get(),
                                  bb * kStamps * sizeof(UINT64));
            timed[bb] = true;
        }
        cmd->Close();

        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);
        cmd = nullptr;
        const HRESULT hr = swapChain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        frameFence[bb] = ++fenceValue;
        queue->Signal(fence.Get(), frameFence[bb]);

        publishStatus();
        // Keep the decoder surface alive until the GPU has sampled it.
        inFlight[bb] = av_frame_clone(&frame);
        if (last != &frame) {
            av_frame_free(&last);
            last = av_frame_clone(&frame);
        }
        if (shot) {
            waitIdle();
            screenshotOk = shotReadback && writeScreenshot(shotBox.right - shotBox.left, shotBox.bottom - shotBox.top,
                                                           static_cast<UINT>(shotPitch));
            screenshotDone = true;
        }
        if (FAILED(hr)) {
            log::error("presenter: Present failed 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        return true;
    }

    // Readback -> 8-bit BGRA -> PNG through WIC. An HDR (scRGB FP16) frame is brought to SDR
    // with a simple highlight roll-off so the file still looks right on any viewer.
    bool writeScreenshot(UINT w, UINT h, UINT pitch) {
        uint8_t* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(pitch) * h};
        if (FAILED(shotReadback->Map(0, &range, reinterpret_cast<void**>(&data)))) return false;
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        if (bbFormat == kHdrFormat) {
            const auto half = [](uint16_t v) {
                const uint32_t sign = (v >> 15) & 1, exp = (v >> 10) & 31, mant = v & 1023;
                float f = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
                          : exp == 31 ? 65504.0f
                                      : std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
                return sign ? -f : f;
            };
            const auto encode = [](float linear) {
                linear = linear / (1.0f + std::max(0.0f, linear - 1.0f) * 0.25f); // gentle roll-off above SDR white
                linear = std::clamp(linear, 0.0f, 1.0f);
                const float s = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                return static_cast<uint8_t>(std::lround(s * 255.0f));
            };
            for (UINT y = 0; y < h; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(data + static_cast<size_t>(y) * pitch);
                for (UINT x = 0; x < w; ++x) {
                    auto* o = &bgra[(static_cast<size_t>(y) * w + x) * 4];
                    o[0] = encode(half(row[x * 4 + 2]));
                    o[1] = encode(half(row[x * 4 + 1]));
                    o[2] = encode(half(row[x * 4 + 0]));
                    o[3] = 255;
                }
            }
        } else {
            for (UINT y = 0; y < h; ++y) {
                std::memcpy(&bgra[static_cast<size_t>(y) * w * 4], data + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
                for (UINT x = 0; x < w; ++x) bgra[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
            }
        }
        const D3D12_RANGE none{0, 0};
        shotReadback->Unmap(0, &none);
        return savePng(screenshotPath, w, h, bgra.data());
    }

    ngx::TrueHdrParams hdrParams() const {
        auto params = settings.hdr;
        if (params.peakNits == 0) params.peakNits = hdrPeak > 0 ? static_cast<uint32_t>(hdrPeak) : 1000;
        return ngx::TrueHdr::clamp(params);
    }
};

std::unique_ptr<D3D12Presenter> D3D12Presenter::create(gpu::D3D12Context& gpu, ngx::NgxRuntime* ngx,
                                                       telemetry::Telemetry* telemetry, enhance::NrLocator nrLocator) {
    auto impl = std::make_unique<Impl>();
    impl->gpu = &gpu;
    impl->device = gpu.device();
    impl->queue = gpu.directQueue();
    impl->ngx = ngx && ngx->initialized() ? ngx : nullptr;
    impl->nrLocator = std::move(nrLocator);
    if (telemetry) {
        impl->gpuStage = &telemetry->stage("gpu.frame");
        impl->convertStage = &telemetry->stage("gpu.convert");
        impl->nrStage = &telemetry->stage("gpu.nr");
        impl->nrCompositeStage = &telemetry->stage("gpu.nr.composite");
        impl->vsrStage = &telemetry->stage("gpu.vsr");
        impl->hdrStage = &telemetry->stage("gpu.truehdr");
    }
    if (!impl->device || !impl->queue || !impl->init()) return nullptr;
    return std::unique_ptr<D3D12Presenter>(new D3D12Presenter(std::move(impl)));
}

D3D12Presenter::D3D12Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) { impl_->startWorker(); }
D3D12Presenter::~D3D12Presenter() {
    if (impl_) impl_->stopWorker();
}

unsigned D3D12Presenter::interpolationFactor() const {
    return impl_->factor.load();
}

bool D3D12Presenter::presentInterpolated(const AVFrame& next, float t) {
    std::lock_guard lock(impl_->mutex);
    if (t <= 0.0f || t >= 1.0f) return false;
    return impl_->draw(next, t);
}

bool D3D12Presenter::screenshot(const std::string& path) {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->last) return false;
        impl_->screenshotPath = path;
        impl_->screenshotDone = false;
        impl_->screenshotOk = false;
    }
    // Paused or idle: draw the last frame now. Playing: the next frame serves it.
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->screenshotDone) {
                impl_->screenshotPath.clear();
                return impl_->screenshotOk;
            }
        }
        if (i == 10) redraw();
        Sleep(5);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->screenshotPath.clear();
    return impl_->screenshotOk;
}

bool D3D12Presenter::present(const AVFrame& frame) {
    std::lock_guard lock(impl_->mutex);
    return impl_->draw(frame);
}

void D3D12Presenter::redraw() {
    std::lock_guard lock(impl_->mutex);
    impl_->redrawLast();
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
    impl_->requestRedraw(/*force=*/true);
}

IDXGISwapChain1* D3D12Presenter::swapChain() const { return impl_->swapChain.Get(); }

void D3D12Presenter::setEnhance(const EnhanceSettings& settings) {
    // Never waits for the render path (which may be building an NGX feature for a second or two):
    // the settings are handed over and picked up by the next frame, or by the redraw worker.
    {
        std::lock_guard lock(impl_->shared);
        impl_->pending = settings;
        impl_->factor = std::clamp<uint32_t>(settings.interpolation, 1, 4);
    }
    impl_->requestRedraw();
}

EnhanceSettings D3D12Presenter::enhance() const {
    std::lock_guard lock(impl_->shared);
    return impl_->pending ? *impl_->pending : impl_->published;
}

EnhanceStatus D3D12Presenter::enhanceStatus() const {
    std::lock_guard lock(impl_->shared);
    return impl_->statusCopy;
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
