#include "GpuEnhancer.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>

namespace bv::exporter {

using Microsoft::WRL::ComPtr;

namespace {

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

class NrFilter final : public FrameFilter {
public:
    ~NrFilter() override {
        wait();
        nr_.reset(); // NR releases its feature before the queue goes away
        if (event_) CloseHandle(event_);
    }

    bool init(ID3D12Device* device, ngx::NgxRuntime* ngx, enhance::NrLocator locate, uint32_t w, uint32_t h, bool neural,
              const enhance::NeuralControls& controls, uint32_t denoise, const enhance::ColorControls& color,
              double frameMs, std::string* error) {
        denoiseLevel_ = denoise;
        color_ = color;
        if (!color.identity()) {
            postfx_ = enhance::PostFx::create(device, w, h);
            if (!postfx_) return fail(error, "no se pudo crear el ajuste de color");
        }
        device_ = device;
        width_ = w;
        height_ = h;
        controls_ = controls;
        frameMs_ = frameMs;
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return fail(error, "sin cola de GPU");
        for (int i = 0; i < 2; ++i) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i]))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[i].Get(), nullptr,
                                                 IID_PPV_ARGS(&list_[i]))))
                return fail(error, "sin listas de comandos");
            list_[i]->Close();
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return fail(error, "sin fence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&source_))))
            return fail(error, "sin memoria de video");
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &footprint_, nullptr, nullptr, &total);
        D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD}, rb{D3D12_HEAP_TYPE_READBACK};
        if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&upload_))) ||
            FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback_))))
            return fail(error, "sin memoria para la transferencia");
        // Persistently mapped: the CPU writes/reads only while the GPU is idle on this queue.
        const D3D12_RANGE none{0, 0};
        if (FAILED(upload_->Map(0, &none, reinterpret_cast<void**>(&uploadPtr_))) ||
            FAILED(readback_->Map(0, nullptr, reinterpret_cast<void**>(&readbackPtr_))))
            return fail(error, "no se pudo mapear la memoria");

        if (denoise) {
            denoise_ = enhance::Denoise::create(device, w, h);
            if (!denoise_) return fail(error, "no se pudo crear la reducción de ruido");
        }
        if (!neural) return true;
        alloc_[0]->Reset();
        list_[0]->Reset(alloc_[0].Get(), nullptr);
        std::string reason;
        nr_ = enhance::NeuralRendering::create(ngx, device, list_[0].Get(), locate, w, h, controls, &reason);
        list_[0]->Close();
        submit(list_[0].Get());
        wait();
        if (!nr_) return fail(error, reason);
        log::info("export: DLSS NR {} at {}x{}", nr_->label(), nr_->nrWidth(), nr_->nrHeight());
        return true;
    }

    bool process(const uint8_t* rgba, int stride, uint8_t* out, int outStride) override {
        const auto rowBytes = static_cast<size_t>(width_) * 4;
        for (uint32_t y = 0; y < height_; ++y)
            std::memcpy(uploadPtr_ + footprint_.Offset + y * footprint_.Footprint.RowPitch, rgba + static_cast<size_t>(y) * stride,
                        rowBytes);

        // List A: upload and NR input. Submitted alone: NR's evaluation needs a fresh list.
        auto* a = list_[0].Get();
        alloc_[0]->Reset();
        a->Reset(alloc_[0].Get(), nullptr);
        if (!first_) {
            const auto b = transition(source_.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
            a->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION src{upload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = footprint_;
        D3D12_TEXTURE_COPY_LOCATION dst{source_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        a->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const auto toRead = transition(source_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        a->ResourceBarrier(1, &toRead);
        ID3D12Resource* base = source_.Get();
        if (denoise_) {
            denoise_->run(a, base, 1.0f, 1.0f, static_cast<float>(denoiseLevel_));
            base = denoise_->output();
        }
        bool ok = true;
        const auto pitch = footprint_.Footprint.RowPitch;
        if (!nr_) {
            // No NR: denoise and/or colour, read back from this list.
            if (postfx_) {
                postfx_->run(a, base, color_);
                postfx_->copyOutput(a, readback_.Get(), footprint_.Offset, pitch);
            } else {
                denoise_->copyOutput(a, readback_.Get(), footprint_.Offset, pitch);
            }
            if (denoise_ && postfx_) denoise_->finish(a);
            a->Close();
            submit(a);
            wait();
        } else {
            nr_->prepare(a, base);
            a->Close();
            submit(a);

            // List B: NR, anti-flicker, composite, readback.
            auto* b = list_[1].Get();
            alloc_[1]->Reset();
            b->Reset(alloc_[1].Get(), nullptr);
            ok = nr_->evaluate(b, controls_, first_, frameMs_);
            if (ok) {
                nr_->composite(b, base, controls_);
                if (postfx_) {
                    postfx_->run(b, nr_->output(), color_);
                    postfx_->copyOutput(b, readback_.Get(), footprint_.Offset, pitch);
                    nr_->finish(b);
                } else {
                    nr_->copyOutput(b, readback_.Get(), footprint_.Offset, pitch);
                }
            }
            if (denoise_) denoise_->finish(b);
            b->Close();
            submit(b);
            wait();
        }
        first_ = false;
        if (!ok) return false;
        for (uint32_t y = 0; y < height_; ++y)
            std::memcpy(out + static_cast<size_t>(y) * outStride,
                        readbackPtr_ + footprint_.Offset + y * footprint_.Footprint.RowPitch, rowBytes);
        return true;
    }

    std::string describe() const override {
        std::string out;
        if (denoise_) out = "Reducción de ruido";
        if (postfx_) out += (out.empty() ? "" : " · ") + std::string("Color");
        if (nr_) out += (out.empty() ? "" : " · ") + std::string("DLSS NR ") + nr_->label();
        return out;
    }

private:
    static bool fail(std::string* error, std::string why) {
        if (error) *error = std::move(why);
        return false;
    }

    void submit(ID3D12GraphicsCommandList* list) {
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
    }

    void wait() {
        if (!queue_ || !fence_) return;
        const UINT64 v = ++fenceValue_;
        queue_->Signal(fence_.Get(), v);
        if (fence_->GetCompletedValue() < v) {
            fence_->SetEventOnCompletion(v, event_);
            WaitForSingleObject(event_, 10000);
        }
    }

    ID3D12Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    enhance::NeuralControls controls_{};
    double frameMs_ = 0;
    bool first_ = true;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> alloc_[2];
    ComPtr<ID3D12GraphicsCommandList> list_[2];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
    ComPtr<ID3D12Resource> source_, upload_, readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    uint8_t* uploadPtr_ = nullptr;
    uint8_t* readbackPtr_ = nullptr;
    std::unique_ptr<enhance::NeuralRendering> nr_;
    std::unique_ptr<enhance::Denoise> denoise_;
    uint32_t denoiseLevel_ = 0;
    std::unique_ptr<enhance::PostFx> postfx_;
    enhance::ColorControls color_{};
};

} // namespace

std::unique_ptr<FrameFilter> createNrFilter(ID3D12Device* device, ngx::NgxRuntime* ngx, enhance::NrLocator locate,
                                            uint32_t width, uint32_t height, bool neural,
                                            const enhance::NeuralControls& controls, uint32_t denoise,
                                            const enhance::ColorControls& color, double frameMs, std::string* error) {
    auto filter = std::make_unique<NrFilter>();
    if (!filter->init(device, ngx, std::move(locate), width, height, neural, controls, denoise, color, frameMs, error))
        return nullptr;
    return filter;
}

} // namespace bv::exporter

#else

namespace bv::exporter {
std::unique_ptr<FrameFilter> createNrFilter(ID3D12Device*, ngx::NgxRuntime*, enhance::NrLocator, uint32_t, uint32_t, bool,
                                            const enhance::NeuralControls&, uint32_t, const enhance::ColorControls&, double,
                                            std::string* error) {
    if (error) *error = "DLSS NR solo está disponible en Windows";
    return nullptr;
}
} // namespace bv::exporter

#endif
