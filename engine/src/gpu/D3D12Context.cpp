#include "D3D12Context.h"

#include "core/Log.h"

#if defined(_WIN32)
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>
#endif

namespace bv::gpu {

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

struct D3D12Context::Impl {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> directQueue;  // presentation + graphics
    ComPtr<ID3D12CommandQueue> computeQueue; // enhancement passes
    ComPtr<ID3D12CommandQueue> copyQueue;    // uploads / readbacks
};

namespace {

ComPtr<ID3D12CommandQueue> makeQueue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                     const wchar_t* name) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) queue->SetName(name);
    return queue;
}

} // namespace

std::unique_ptr<D3D12Context> D3D12Context::create(const bv_adapter_info& adapter, bv_result* error) {
    auto fail = [&](bv_result r) {
        if (error) *error = r;
        return std::unique_ptr<D3D12Context>{};
    };

    auto impl = std::make_unique<Impl>();
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&impl->factory)))) return fail(BV_ERR_INTERNAL);
    if (FAILED(impl->factory->EnumAdapterByGpuPreference(adapter.index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&impl->adapter)))) {
        return fail(BV_ERR_NO_COMPATIBLE_GPU);
    }
    if (FAILED(D3D12CreateDevice(impl->adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&impl->device)))) {
        return fail(BV_ERR_NO_COMPATIBLE_GPU);
    }
    impl->device->SetName(L"Blackvolt Device");

    impl->directQueue = makeQueue(impl->device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"bv.direct");
    impl->computeQueue = makeQueue(impl->device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, L"bv.compute");
    impl->copyQueue = makeQueue(impl->device.Get(), D3D12_COMMAND_LIST_TYPE_COPY, L"bv.copy");
    if (!impl->directQueue || !impl->computeQueue || !impl->copyQueue) return fail(BV_ERR_INTERNAL);

    log::info("D3D12 device ready on {}", adapter.name);
    if (error) *error = BV_OK;
    return std::unique_ptr<D3D12Context>(new D3D12Context(adapter, std::move(impl)));
}

bool D3D12Context::isDeviceLost() const {
    return impl_ && impl_->device && FAILED(impl_->device->GetDeviceRemovedReason());
}

#else

struct D3D12Context::Impl {};

std::unique_ptr<D3D12Context> D3D12Context::create(const bv_adapter_info&, bv_result* error) {
    if (error) *error = BV_ERR_UNSUPPORTED;
    return {};
}

bool D3D12Context::isDeviceLost() const { return false; }

#endif

D3D12Context::D3D12Context(const bv_adapter_info& adapter, std::unique_ptr<Impl> impl)
    : adapter_(adapter), impl_(std::move(impl)) {}

D3D12Context::~D3D12Context() = default;

} // namespace bv::gpu
