#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace bv::player {

// Master clock for A/V sync. When audio is playing, the audio sink drives it by reporting
// what it has actually played; otherwise it runs on the steady clock.
class MediaClock {
public:
    using Clock = std::chrono::steady_clock;

    void reset(double positionSeconds) {
        std::lock_guard lock(mutex_);
        anchorPos_ = positionSeconds;
        anchorTime_ = Clock::now();
    }

    void setPaused(bool paused) {
        std::lock_guard lock(mutex_);
        if (paused == paused_) return;
        anchorPos_ = nowLocked();
        anchorTime_ = Clock::now();
        paused_ = paused;
    }

    // Audio sink: "the sample with timestamp `positionSeconds` is being heard right now".
    void syncTo(double positionSeconds) {
        std::lock_guard lock(mutex_);
        // Small corrections are smoothed to avoid visible jumps; large ones snap.
        const double drift = positionSeconds - nowLocked();
        anchorPos_ = nowLocked() + (std::fabs(drift) > 0.2 ? drift : drift * 0.1);
        anchorTime_ = Clock::now();
    }

    [[nodiscard]] double now() const {
        std::lock_guard lock(mutex_);
        return nowLocked();
    }

    [[nodiscard]] bool paused() const {
        std::lock_guard lock(mutex_);
        return paused_;
    }

    void setRate(double rate) {
        std::lock_guard lock(mutex_);
        anchorPos_ = nowLocked();
        anchorTime_ = Clock::now();
        rate_ = rate;
    }

private:
    double nowLocked() const {
        if (paused_) return anchorPos_;
        return anchorPos_ + std::chrono::duration<double>(Clock::now() - anchorTime_).count() * rate_;
    }

    mutable std::mutex mutex_;
    double anchorPos_ = 0.0;
    Clock::time_point anchorTime_ = Clock::now();
    double rate_ = 1.0;
    bool paused_ = true;
};

// Decides what the render loop does with the next frame. Pure function: unit tested.
enum class FrameAction { Wait, Present, Drop };

// framePts: presentation time of the frame at the head of the queue.
// nextPts:  presentation time of the frame after it (NaN if unknown).
// clock:    current master clock. tolerance: half a display refresh.
inline FrameAction scheduleFrame(double framePts, double nextPts, double clock, double tolerance) {
    if (framePts > clock + tolerance) return FrameAction::Wait;
    // Late: if the following frame is also already due, this one would never be seen.
    if (!std::isnan(nextPts) && nextPts <= clock + tolerance) return FrameAction::Drop;
    return FrameAction::Present;
}

} // namespace bv::player
