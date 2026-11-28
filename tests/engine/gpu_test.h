#pragma once
// Shared helpers for tests that run compute work on a real D3D12 device (hardware GPU, or WARP
// on machines without one, such as CI runners).
#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace gputest {

using Microsoft::WRL::ComPtr;

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

inline void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &x);
}

struct Device {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    std::vector<ComPtr<ID3D12Resource>> keep; // staging buffers alive until the next run()

    bool open() {
        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            ComPtr<IDXGIAdapter> warp;
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
                FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return queue && list && fence && event;
    }

    void run() {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 60000);
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        keep.clear();
    }

    // RGBA8 texture from tightly packed pixels, left in shader-read state (after run()).
    ComPtr<ID3D12Resource> upload(const std::vector<uint8_t>& rgba, UINT w, UINT h) {
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> tex, buf;
        device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
        device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
        uint8_t* p = nullptr;
        buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < h; ++y) std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, &rgba[y * w * 4], w * 4);
        buf->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION s{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        s.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION d{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        keep.push_back(buf);
        return tex;
    }

    // Reads back an RGBA8 texture that is in `state`; runs the list.
    std::vector<uint8_t> download(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, UINT w, UINT h) {
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> buf;
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
        barrier(list.Get(), tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION s{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION d{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        d.PlacedFootprint = fp;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(list.Get(), tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        run();
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
        uint8_t* p = nullptr;
        buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < h; ++y) std::memcpy(&out[y * w * 4], p + fp.Offset + y * fp.Footprint.RowPitch, w * 4);
        buf->Unmap(0, nullptr);
        return out;
    }

    ~Device() {
        if (event) CloseHandle(event);
    }
};

} // namespace gputest
#endif
