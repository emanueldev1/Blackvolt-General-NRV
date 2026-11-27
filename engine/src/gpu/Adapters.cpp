#include "Adapters.h"

#include "AdapterRanking.h"
#include "core/Log.h"

#if defined(_WIN32)
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>
#  include <cstring>
#  include <string>
#endif

namespace bv::gpu {

#if defined(_WIN32)
namespace {

using Microsoft::WRL::ComPtr;

std::string toUtf8(const wchar_t* wide) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

bool tearingSupported(IDXGIFactory6* factory) {
    BOOL allow = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5))) &&
        SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
        return allow == TRUE;
    }
    return false;
}

} // namespace

std::vector<bv_adapter_info> enumerateAdapters() {
    std::vector<bv_adapter_info> result;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        log::error("CreateDXGIFactory2 failed");
        return result;
    }
    const bool tearing = tearingSupported(factory.Get());

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // Probe only: a null output pointer checks support without creating the device.
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                     nullptr))) {
            continue;
        }

        bv_adapter_info info{};
        info.size = sizeof(info);
        info.index = i;
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.vendor = vendorFromId(desc.VendorId);
        info.dedicated_vram_bytes = desc.DedicatedVideoMemory;

        const std::string name = toUtf8(desc.Description);
        std::strncpy(info.name, name.c_str(), sizeof(info.name) - 1);

        info.capabilities = inferCapabilities(desc.VendorId, name);
        if (tearing) info.capabilities |= BV_CAP_TEARING;

        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)) &&
            desc.VendorId == kVendorNvidia) {
            nvidiaDriverVersion(static_cast<uint64_t>(umd.QuadPart), info.driver_version,
                                sizeof(info.driver_version));
        }
        result.push_back(info);
    }
    return result;
}

#else

std::vector<bv_adapter_info> enumerateAdapters() { return {}; }

#endif

} // namespace bv::gpu
