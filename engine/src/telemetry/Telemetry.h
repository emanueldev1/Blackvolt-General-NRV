#pragma once

#include "RollingStats.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>

namespace bv::telemetry {

// ~4 seconds of history at 240 fps.
inline constexpr std::size_t kWindow = 1024;
using StageStats = RollingStats<kWindow>;

// Registry of named stages ("decode", "enhance", "present", "frame").
// Stages are created on first use and live as long as the registry.
class Telemetry {
public:
    StageStats& stage(std::string_view name);
    [[nodiscard]] const StageStats* find(std::string_view name) const;
    void resetAll();

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<StageStats>> stages_;
};

// RAII scope timer: records elapsed microseconds into a stage on destruction.
class ScopedTimer {
public:
    explicit ScopedTimer(StageStats& target)
        : target_(target), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        target_.record(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    StageStats& target_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace bv::telemetry
