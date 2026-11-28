// Frame interpolation on a real D3D12 device (hardware GPU, or WARP on machines without one).
// Frame B is frame A moved by a known amount; the generated middle frame must match A moved by
// half of it far better than a cross-fade does.
#include "check.h"

#if defined(_WIN32)

#include "enhance/FrameInterpolation.h"
#include "gpu_test.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;
using gputest::Device;
using gputest::barrier;

namespace {

constexpr UINT kW = 640, kH = 360;

double hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
}

double noise(double x, double y, double cell) {
    const double gx = x / cell, gy = y / cell;
    const int ix = static_cast<int>(std::floor(gx)), iy = static_cast<int>(std::floor(gy));
    double fx = gx - ix, fy = gy - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    return (hash(ix, iy) * (1 - fx) + hash(ix + 1, iy) * fx) * (1 - fy) +
           (hash(ix, iy + 1) * (1 - fx) + hash(ix + 1, iy + 1) * fx) * fy;
}

// Non-periodic, multi-scale texture: a periodic pattern would make some shifts ambiguous.
double texture(double x, double y) {
    return std::clamp(0.55 * noise(x, y, 37) + 0.30 * noise(x + 1000, y, 11) + 0.15 * noise(x, y + 2000, 4), 0.0, 1.0);
}

// Mean absolute error (0..255) of the generated middle frame, and of a cross-fade, for motion (dx, dy).
bool measure(Device& g, int dx, int dy, double& interp, double& fade) {
    auto fi = bv::enhance::FrameInterpolation::create(g.device.Get(), kW, kH);
    if (!fi) return false;
    const D3D12_RESOURCE_STATES read =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD}, rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kW, kH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> tex[2], upload[2], readback;
    if (FAILED(g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback))))
        return false;
    for (int i = 0; i < 2; ++i) {
        g.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&tex[i]));
        g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&upload[i]));
        uint8_t* p = nullptr;
        upload[i]->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const auto v = static_cast<uint8_t>(texture(static_cast<int>(x) - (i ? dx : 0), static_cast<int>(y) - (i ? dy : 0)) * 255);
                uint8_t* o = p + fp.Offset + y * fp.Footprint.RowPitch + x * 4;
                o[0] = v;
                o[1] = static_cast<uint8_t>(255 - v);
                o[2] = static_cast<uint8_t>(v / 2 + 60);
                o[3] = 255;
            }
        upload[i]->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION s{upload[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        s.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION d{tex[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        g.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(g.list.Get(), tex[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    }
    fi->ingest(g.list.Get(), 0, tex[0].Get());
    fi->ingest(g.list.Get(), 1, tex[1].Get());
    fi->interpolate(g.list.Get(), 0, 1, 0.5f);
    barrier(g.list.Get(), fi->output(), read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s{fi->output(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION d{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    d.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    barrier(g.list.Get(), fi->output(), D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    g.run();

    uint8_t* p = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&p));
    interp = fade = 0;
    long n = 0;
    const int margin = 48;
    for (int y = margin; y < static_cast<int>(kH) - margin; ++y)
        for (int x = margin; x < static_cast<int>(kW) - margin; ++x) {
            const double truth = texture(x - dx / 2.0, y - dy / 2.0) * 255;
            interp += std::abs(p[fp.Offset + y * fp.Footprint.RowPitch + x * 4] - truth);
            fade += std::abs(0.5 * (texture(x, y) + texture(x - dx, y - dy)) * 255 - truth);
            ++n;
        }
    readback->Unmap(0, nullptr);
    interp /= n;
    fade /= n;
    return true;
}

} // namespace

TEST(frame_interpolation_follows_motion) {
    Device g;
    if (!g.open()) {
        std::printf("  (sin dispositivo D3D12: omitido)\n");
        return;
    }
    const int motions[][2] = {{12, 0}, {0, -12}, {24, -12}, {-40, 30}, {6, 3}, {0, 0}};
    for (const auto& m : motions) {
        double interp = 0, fade = 0;
        CHECK(measure(g, m[0], m[1], interp, fade));
        std::printf("  motion (%d,%d): interpolated %.2f, cross-fade %.2f\n", m[0], m[1], interp, fade);
        CHECK(interp < 6.0);
        CHECK(interp <= std::max(fade * 0.5, 1.5));
    }
}

TEST(motion_estimate_points_to_the_other_frame) {
    Device g;
    if (!g.open()) {
        std::printf("  (sin dispositivo D3D12: omitido)\n");
        return;
    }
    const int dx = 20, dy = -10; // frame 1 = frame 0 moved by (dx, dy)
    std::vector<uint8_t> px[2];
    for (int i = 0; i < 2; ++i) {
        px[i].resize(kW * kH * 4);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const auto v = static_cast<uint8_t>(texture(static_cast<int>(x) - (i ? dx : 0), static_cast<int>(y) - (i ? dy : 0)) * 255);
                uint8_t* o = &px[i][(y * kW + x) * 4];
                o[0] = o[1] = o[2] = v;
                o[3] = 255;
            }
    }
    auto fi = bv::enhance::FrameInterpolation::create(g.device.Get(), kW, kH);
    CHECK(fi != nullptr);
    if (!fi) return;
    auto a = g.upload(px[0], kW, kH), b = g.upload(px[1], kW, kH);
    fi->ingest(g.list.Get(), 0, a.Get());
    fi->ingest(g.list.Get(), 1, b.Get());
    // Backward (1 -> 0), as the NR anti-flicker uses it: frame 0 at p + v shows frame 1 at p.
    fi->estimate(g.list.Get(), 1, 0);

    auto* motion = fi->motion();
    const auto desc = motion->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> buf;
    g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
    barrier(g.list.Get(), motion, gputest::kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s{motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION d{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    d.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    barrier(g.list.Get(), motion, D3D12_RESOURCE_STATE_COPY_SOURCE, gputest::kRead);
    g.run();

    auto half = [](uint16_t v) {
        const int e = (v >> 10) & 31, m = v & 1023;
        const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024), e - 25);
        return (v & 0x8000) ? -f : f;
    };
    uint8_t* p = nullptr;
    buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
    double sx = 0, sy = 0;
    long n = 0;
    for (UINT y = 6; y + 6 < desc.Height; ++y)
        for (UINT x = 6; x + 6 < desc.Width; ++x) {
            const auto* v = reinterpret_cast<const uint16_t*>(p + fp.Offset + y * fp.Footprint.RowPitch) + x * 2;
            sx += half(v[0]);
            sy += half(v[1]);
            ++n;
        }
    buf->Unmap(0, nullptr);
    sx /= n;
    sy /= n;
    std::printf("  estimated (%.2f, %.2f), expected (%d, %d)\n", sx, sy, -dx, -dy);
    CHECK(std::abs(sx + dx) < 1.0);
    CHECK(std::abs(sy + dy) < 1.0);
}

#endif
