#pragma once

#include "blackvolt/bv_api.h"

#include <memory>

namespace bv::gpu {

// Owns the D3D12 device and the queues the pipeline submits to.
// Separate queues let decode, enhancement and presentation overlap on the GPU.
class D3D12Context {
public:
    static std::unique_ptr<D3D12Context> create(const bv_adapter_info& adapter, bv_result* error);
    ~D3D12Context();

    D3D12Context(const D3D12Context&) = delete;
    D3D12Context& operator=(const D3D12Context&) = delete;

    [[nodiscard]] const bv_adapter_info& adapter() const { return adapter_; }
    [[nodiscard]] bool isDeviceLost() const;

    struct Impl;

private:
    D3D12Context(const bv_adapter_info& adapter, std::unique_ptr<Impl> impl);

    bv_adapter_info adapter_;
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::gpu
