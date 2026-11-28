// Video denoise on a real D3D12 device (or WARP): noise goes down, edges and flat colour stay.
#include "check.h"

#if defined(_WIN32)

#include "enhance/Denoise.h"
#include "gpu_test.h"

#include <algorithm>
#include <cmath>
#include <random>

TEST(denoise_removes_noise_and_keeps_edges) {
    gputest::Device g;
    if (!g.open()) {
        std::printf("  (sin dispositivo D3D12: omitido)\n");
        return;
    }
    constexpr UINT w = 256, h = 128;
    // Clean picture: two flat halves with a hard vertical edge in the middle.
    auto clean = [&](UINT x, UINT, int c) { return x < w / 2 ? (c == 0 ? 60 : c == 1 ? 90 : 120) : (c == 0 ? 190 : c == 1 ? 170 : 150); };
    std::vector<uint8_t> noisy(w * h * 4);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 6.0);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                noisy[(y * w + x) * 4 + c] = c == 3 ? 255 : static_cast<uint8_t>(std::clamp(clean(x, y, c) + noise(rng), 0.0, 255.0));

    auto stage = bv::enhance::Denoise::create(g.device.Get(), w, h);
    CHECK(stage != nullptr);
    if (!stage) return;
    auto input = g.upload(noisy, w, h);
    // Gentle (Veyra's calibration) must already help; strong must remove most of the noise.
    stage->run(g.list.Get(), input.Get(), 1.0f, 1.0f, 1.0f);
    const auto gentle = g.download(stage->output(), gputest::kRead, w, h);
    stage->run(g.list.Get(), input.Get(), 1.0f, 1.0f, 3.0f);
    const auto out = g.download(stage->output(), gputest::kRead, w, h);

    double before = 0, soft = 0, after = 0, edge = 0;
    long n = 0;
    for (UINT y = 4; y < h - 4; ++y)
        for (UINT x = 4; x < w - 4; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (y * w + x) * 4 + c;
                before += std::abs(noisy[i] - clean(x, y, c));
                soft += std::abs(gentle[i] - clean(x, y, c));
                after += std::abs(out[i] - clean(x, y, c));
                ++n;
            }
    for (UINT y = 4; y < h - 4; ++y) // the edge itself must stay sharp: neighbours keep their side
        edge += std::abs(out[(y * w + w / 2 - 1) * 4] - 60) + std::abs(out[(y * w + w / 2) * 4] - 190);
    before /= n;
    soft /= n;
    after /= n;
    edge /= 2.0 * (h - 8);
    std::printf("  noise error %.2f -> gentle %.2f, strong %.2f; edge error %.2f\n", before, soft, after, edge);
    CHECK(soft < before * 0.9);
    CHECK(after < before * 0.6);
    CHECK(edge < 8.0);
}

#endif
