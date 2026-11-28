#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bv::telemetry {

struct StatsSnapshot {
    uint32_t samples = 0;
    double avg = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// Fixed-capacity window of the most recent samples. One writer (the stage that
// records), any number of readers (UI polling). Recording is O(1) and never allocates.
template <std::size_t Capacity>
class RollingStats {
    static_assert(Capacity > 0, "Capacity must be positive");

public:
    void record(double value) {
        std::lock_guard lock(mutex_);
        ring_[head_] = value;
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    void reset() {
        std::lock_guard lock(mutex_);
        head_ = 0;
        count_ = 0;
    }

    // Copies up to `capacity` of the most recent samples, oldest first. Returns how many.
    std::size_t recent(float* out, std::size_t capacity) const {
        std::lock_guard lock(mutex_);
        const std::size_t n = std::min(capacity, count_);
        if (!out) return n;
        const std::size_t first = (head_ + Capacity - n) % Capacity;
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(ring_[(first + i) % Capacity]);
        return n;
    }

    [[nodiscard]] StatsSnapshot snapshot() const {
        std::vector<double> sorted;
        {
            std::lock_guard lock(mutex_);
            sorted.assign(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(count_));
        }
        StatsSnapshot s;
        s.samples = static_cast<uint32_t>(sorted.size());
        if (sorted.empty()) return s;

        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double v : sorted) sum += v;
        s.avg = sum / static_cast<double>(sorted.size());
        s.p50 = percentile(sorted, 0.50);
        s.p95 = percentile(sorted, 0.95);
        s.p99 = percentile(sorted, 0.99);
        s.max = sorted.back();
        return s;
    }

private:
    // Nearest-rank percentile on an already sorted, non-empty vector.
    static double percentile(const std::vector<double>& sorted, double p) {
        const auto n = sorted.size();
        auto rank = static_cast<std::size_t>(p * static_cast<double>(n) + 0.999999);
        rank = std::clamp<std::size_t>(rank, 1, n);
        return sorted[rank - 1];
    }

    mutable std::mutex mutex_;
    std::array<double, Capacity> ring_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

} // namespace bv::telemetry
