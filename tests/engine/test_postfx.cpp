// Colour controls and FXAA on a real D3D12 device (or WARP).
#include "check.h"

#if defined(_WIN32)

#include "enhance/PostFx.h"
#include "gpu_test.h"

#include <algorithm>
#include <cmath>

TEST(postfx_identity_saturation_and_fxaa) {
    gputest::Device g;
    if (!g.open()) {
        std::printf("  (sin dispositivo D3D12: omitido)\n");
        return;
    }
    constexpr UINT w = 128, h = 128;
    // A colour gradient, plus a hard aliased diagonal in the top-left quarter.
    std::vector<uint8_t> px(w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            uint8_t* o = &px[(y * w + x) * 4];
            if (x < 64 && y < 64) {
                const uint8_t v = (x * 2 > y * 3) ? 255 : 0; // stair-stepped edge
                o[0] = o[1] = o[2] = v;
            } else {
                o[0] = static_cast<uint8_t>(x * 2);
                o[1] = static_cast<uint8_t>(y * 2);
                o[2] = 120;
            }
            o[3] = 255;
        }
    auto fx = bv::enhance::PostFx::create(g.device.Get(), w, h);
    CHECK(fx != nullptr);
    if (!fx) return;
    auto input = g.upload(px, w, h);

    // Defaults must leave the picture as it is.
    fx->run(g.list.Get(), input.Get(), {});
    auto out = g.download(fx->output(), gputest::kRead, w, h);
    int worst = 0;
    for (size_t i = 0; i < px.size(); ++i) worst = std::max(worst, std::abs(out[i] - px[i]));
    std::printf("  identity: worst difference %d\n", worst);
    CHECK(worst <= 1);

    // Saturation 0: grey.
    bv::enhance::ColorControls grey;
    grey.saturation = 0;
    fx->run(g.list.Get(), input.Get(), grey);
    out = g.download(fx->output(), gputest::kRead, w, h);
    int spread = 0;
    for (UINT i = 0; i < w * h; ++i)
        spread = std::max(spread, std::max({out[i * 4], out[i * 4 + 1], out[i * 4 + 2]}) - std::min({out[i * 4], out[i * 4 + 1], out[i * 4 + 2]}));
    std::printf("  saturation 0: largest channel spread %d\n", spread);
    CHECK(spread <= 2);

    // Brightness +1 stop makes mid tones brighter; warm temperature raises red over blue.
    bv::enhance::ColorControls warm;
    warm.brightness = 1;
    warm.temperature = 1;
    fx->run(g.list.Get(), input.Get(), warm);
    out = g.download(fx->output(), gputest::kRead, w, h);
    const size_t mid = (100 * w + 100) * 4;
    std::printf("  +1 stop warm at (100,100): %d,%d,%d from %d,%d,%d\n", out[mid], out[mid + 1], out[mid + 2], px[mid], px[mid + 1], px[mid + 2]);
    CHECK(out[mid + 1] > px[mid + 1]);
    CHECK(out[mid] - out[mid + 2] > px[mid] - px[mid + 2]);

    // FXAA: the stair-stepped edge gets in-between values, flat areas stay.
    bv::enhance::ColorControls aa;
    aa.fxaa = true;
    fx->run(g.list.Get(), input.Get(), aa);
    out = g.download(fx->output(), gputest::kRead, w, h);
    int softened = 0, flatChanged = 0;
    for (UINT y = 4; y < 60; ++y)
        for (UINT x = 4; x < 60; ++x) {
            const int v = out[(y * w + x) * 4];
            const bool nearEdge = std::abs(static_cast<int>(x * 2) - static_cast<int>(y * 3)) <= 3;
            if (nearEdge && v > 20 && v < 235) ++softened;
            if (!nearEdge && std::abs(v - px[(y * w + x) * 4]) > 2 && std::abs(static_cast<int>(x * 2) - static_cast<int>(y * 3)) > 8) ++flatChanged;
        }
    std::printf("  fxaa: %d edge pixels softened, %d flat pixels changed\n", softened, flatChanged);
    CHECK(softened > 20);
    CHECK(flatChanged == 0);
}

#endif
