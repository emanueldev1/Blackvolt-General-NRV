#include "TimeStretch.h"

#include <algorithm>
#include <cmath>

namespace bv::audio {

TimeStretch::TimeStretch(int sampleRate, int channels)
    : rate_hz_(std::max(8000, sampleRate)), channels_(std::max(1, channels)) {
    hop_ = static_cast<std::size_t>(rate_hz_ / 100);   // 10 ms
    delta_ = static_cast<std::size_t>(rate_hz_ / 100); // +-10 ms
    const std::size_t n = 2 * hop_;
    window_.resize(n);
    // Periodic Hann: two half-overlapped windows sum to exactly 1.
    for (std::size_t i = 0; i < n; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / n));
    reset();
}

void TimeStretch::setRate(double rate) { rate_ = std::clamp(rate, 0.25, 4.0); }

void TimeStretch::reset() {
    in_.clear();
    tail_.assign(hop_ * channels_, 0.0f);
    base_ = position_ = pushed_ = 0;
    prevStart_ = -1;
}

// Cross-correlation of the candidate segment start with the natural continuation of the previous
// segment, on a mono mix, coarse (step 4) then refined (+-3).
std::size_t TimeStretch::bestOffset(std::size_t nominal) const {
    if (prevStart_ < 0) return nominal;
    const auto natural = static_cast<std::size_t>(prevStart_ - base_) + hop_;
    const std::size_t lo = nominal > delta_ ? nominal - delta_ : 0;
    const std::size_t hi = nominal + delta_;
    const std::size_t len = hop_;
    const auto ch = static_cast<std::size_t>(channels_);
    auto score = [&](std::size_t start) {
        double s = 0;
        for (std::size_t i = 0; i < len; i += 2) {
            float a = 0, b = 0;
            for (std::size_t c = 0; c < ch; ++c) {
                a += in_[(natural + i) * ch + c];
                b += in_[(start + i) * ch + c];
            }
            s += static_cast<double>(a) * b;
        }
        return s;
    };
    std::size_t best = nominal;
    double bestScore = -1e300;
    for (std::size_t k = lo; k <= hi; k += 4) {
        const double v = score(k);
        if (v > bestScore) { bestScore = v; best = k; }
    }
    const std::size_t from = best > lo + 3 ? best - 3 : lo, to = std::min(hi, best + 3);
    for (std::size_t k = from; k <= to; ++k) {
        const double v = score(k);
        if (v > bestScore) { bestScore = v; best = k; }
    }
    return best;
}

void TimeStretch::emitHop(std::vector<float>& out, std::size_t start) {
    const auto ch = static_cast<std::size_t>(channels_);
    const std::size_t first = out.size();
    out.resize(first + hop_ * ch);
    for (std::size_t i = 0; i < hop_; ++i)
        for (std::size_t c = 0; c < ch; ++c)
            out[first + i * ch + c] = tail_[i * ch + c] + window_[i] * in_[(start + i) * ch + c];
    for (std::size_t i = 0; i < hop_; ++i)
        for (std::size_t c = 0; c < ch; ++c) tail_[i * ch + c] = window_[hop_ + i] * in_[(start + hop_ + i) * ch + c];
}

double TimeStretch::process(const float* in, std::size_t frames, std::vector<float>& out) {
    const auto ch = static_cast<std::size_t>(channels_);
    if (rate_ == 1.0 && in_.empty() && prevStart_ < 0) {
        // Pass-through (and nothing buffered from an earlier stretched stretch).
        const double first = pushed_;
        out.insert(out.end(), in, in + frames * ch);
        pushed_ += static_cast<double>(frames);
        base_ = position_ = pushed_;
        return first;
    }
    in_.insert(in_.end(), in, in + frames * ch);
    pushed_ += static_cast<double>(frames);

    const double firstOut = position_;
    const double analysisHop = static_cast<double>(hop_) * rate_;
    for (;;) {
        const auto nominal = static_cast<std::size_t>(std::llround(position_ - base_));
        // Need the segment, the search room and the natural continuation in the buffer.
        const std::size_t need = std::max(nominal + delta_, prevStart_ < 0 ? 0 : static_cast<std::size_t>(prevStart_ - base_) + hop_) + 2 * hop_;
        if (in_.size() / ch < need + 1) break;
        const std::size_t start = bestOffset(nominal);
        emitHop(out, start);
        prevStart_ = base_ + static_cast<double>(start);
        position_ += analysisHop;
    }
    // Drop input no longer reachable (keep the search room and the natural continuation).
    const double keepFrom = std::min(position_ - static_cast<double>(delta_), prevStart_ < 0 ? position_ : prevStart_);
    const auto drop = static_cast<std::size_t>(std::max(0.0, std::floor(keepFrom - base_)));
    if (drop > 0 && drop * ch <= in_.size()) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(drop * ch));
        base_ += static_cast<double>(drop);
    }
    return firstOut;
}

} // namespace bv::audio
