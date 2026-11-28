// Playback speed: duration scales with the rate, pitch and loudness stay.
#include "check.h"

#include "audio/TimeStretch.h"

#include <cmath>
#include <vector>

namespace {
struct Measured { double seconds, hz, rms; };

Measured run(double rate, std::size_t chunk) {
    constexpr int sr = 48000, ch = 2;
    constexpr double f = 440.0;
    std::vector<float> in(static_cast<std::size_t>(sr) * 3 * ch);
    for (std::size_t i = 0; i < in.size() / ch; ++i) {
        const auto v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * f * i / sr));
        in[i * ch] = v;
        in[i * ch + 1] = v;
    }
    bv::audio::TimeStretch ts(sr, ch);
    ts.setRate(rate);
    std::vector<float> out;
    for (std::size_t at = 0; at < in.size() / ch; at += chunk)
        ts.process(in.data() + at * ch, std::min(chunk, in.size() / ch - at), out);
    const std::size_t frames = out.size() / ch;
    // Pitch from zero crossings, loudness from RMS, both away from the edges.
    std::size_t crossings = 0;
    double sum = 0;
    const std::size_t from = sr / 10, to = frames > sr / 10 ? frames - sr / 10 : from;
    for (std::size_t i = from + 1; i < to; ++i) {
        if ((out[(i - 1) * ch] < 0) != (out[i * ch] < 0)) ++crossings;
        sum += static_cast<double>(out[i * ch]) * out[i * ch];
    }
    const double span = static_cast<double>(to - from) / sr;
    return {static_cast<double>(frames) / sr, crossings / 2.0 / span, std::sqrt(sum / std::max<std::size_t>(1, to - from))};
}
} // namespace

TEST(time_stretch_keeps_pitch) {
    for (double rate : {0.5, 0.75, 1.0, 1.5, 2.0, 3.0}) {
        const auto m = run(rate, 1024);
        std::printf("  x%.2f: %.3f s (expected %.3f), %.1f Hz, rms %.3f\n", rate, m.seconds, 3.0 / rate, m.hz, m.rms);
        CHECK(std::abs(m.seconds - 3.0 / rate) < 0.05 * 3.0 / rate + 0.03);
        CHECK(std::abs(m.hz - 440.0) < 440.0 * 0.02);
        CHECK(std::abs(m.rms - 0.3536) < 0.05);
    }
    // Chunk size must not matter.
    const auto a = run(1.5, 333), b = run(1.5, 4096);
    CHECK(std::abs(a.seconds - b.seconds) < 0.03);
}
