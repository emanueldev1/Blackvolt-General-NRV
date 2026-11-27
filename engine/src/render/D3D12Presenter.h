#pragma once

#include "player/Sinks.h"

#include <memory>

namespace bv::gpu { class D3D12Context; }
struct IDXGISwapChain1;

namespace bv::render {

// Presents decoded frames into a composition swapchain that the UI attaches to a
// WinUI SwapChainPanel. GPU frames (D3D12 Video) are sampled in place: zero copies.
class D3D12Presenter final : public player::IVideoPresenter {
public:
    static std::unique_ptr<D3D12Presenter> create(gpu::D3D12Context& gpu);
    ~D3D12Presenter() override;

    bool present(const AVFrame& frame) override;
    void redraw() override;
    [[nodiscard]] double refreshInterval() const override;

    // Physical pixel size of the panel and its DPI scale. Safe to call from the UI thread.
    void resize(uint32_t width, uint32_t height, float scaleX, float scaleY);
    [[nodiscard]] IDXGISwapChain1* swapChain() const;

    struct Impl;

private:
    explicit D3D12Presenter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::render
