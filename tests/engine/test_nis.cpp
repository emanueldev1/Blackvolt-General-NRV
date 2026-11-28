// NVIDIA Image Scaling on a real D3D12 device (or WARP): a 2x upscale of a downsampled picture
// must be at least as close to the original as bilinear, with brightness preserved.
#include "check.h"

#if defined(_WIN32)

#include "enhance/NisScaler.h"
#include "gpu_test.h"

#include <algorithm>
#include <cmath>

namespace {
double original(double x, double y) {
    // Sharp-ish detail: rings and diagonal stripes.
    const double r = std::sqrt((x - 160) * (x - 160) + (y - 90) * (y - 90));
    return std::clamp(0.5 + 0.35 * std::sin(r * 0.35) + 0.15 * (std::fmod(x + y, 24.0) < 12 ? 1 : -1), 0.0, 1.0);
}
} // namespace

TEST(nis_upscales_at_least_as_well_as_bilinear) {
    gputest::Device g;
    if (!g.open()) {
        std::printf("  (sin dispositivo D3D12: omitido)\n");
        return;
    }
    constexpr UINT inW = 160, inH = 90, outW = 320, outH = 180;
    std::vector<uint8_t> low(inW * inH * 4);
    for (UINT y = 0; y < inH; ++y)
        for (UINT x = 0; x < inW; ++x) {
            double v = 0; // box-filter 2x2 of the original
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) v += original(x * 2 + i + 0.5, y * 2 + j + 0.5) / 4;
            for (int c = 0; c < 3; ++c) low[(y * inW + x) * 4 + c] = static_cast<uint8_t>(std::lround(v * 255));
            low[(y * inW + x) * 4 + 3] = 255;
        }
    auto input = g.upload(low, inW, inH);
    auto nis = bv::enhance::NisScaler::create(g.device.Get(), g.list.Get(), inW, inH, outW, outH);
    CHECK(nis != nullptr);
    if (!nis) return;
    g.run(); // coefficient upload
    nis->run(g.list.Get(), input.Get(), 0.25f);
    const auto out = g.download(nis->output(), gputest::kRead, outW, outH);

    auto lowAt = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(inW) - 1);
        y = std::clamp(y, 0, static_cast<int>(inH) - 1);
        return low[(y * inW + x) * 4] / 255.0;
    };
    double errNis = 0, errBilinear = 0, meanNis = 0, meanTruth = 0;
    long n = 0;
    for (UINT y = 8; y < outH - 8; ++y)
        for (UINT x = 8; x < outW - 8; ++x) {
            const double truth = original(x + 0.5, y + 0.5);
            const double sx = (x + 0.5) / 2 - 0.5, sy = (y + 0.5) / 2 - 0.5;
            const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
            const double fx = sx - ix, fy = sy - iy;
            const double bil = (lowAt(ix, iy) * (1 - fx) + lowAt(ix + 1, iy) * fx) * (1 - fy) +
                               (lowAt(ix, iy + 1) * (1 - fx) + lowAt(ix + 1, iy + 1) * fx) * fy;
            const double got = out[(y * outW + x) * 4] / 255.0;
            errNis += std::abs(got - truth);
            errBilinear += std::abs(bil - truth);
            meanNis += got;
            meanTruth += truth;
            ++n;
        }
    errNis = errNis / n * 255;
    errBilinear = errBilinear / n * 255;
    meanNis = meanNis / n * 255;
    meanTruth = meanTruth / n * 255;
    std::printf("  2x error: NIS %.2f vs bilinear %.2f; mean %.1f vs %.1f\n", errNis, errBilinear, meanNis, meanTruth);
    CHECK(errNis <= errBilinear * 1.05);
    CHECK(std::abs(meanNis - meanTruth) < 4.0);
}

#endif
