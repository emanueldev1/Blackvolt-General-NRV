#pragma once

namespace bv {

// While alive, the OS scheduler ticks at 1 ms (Windows defaults to 15.6 ms), so the short
// waits used for frame pacing wake up on time. Reference counted by the OS; no-op elsewhere.
class HighResolutionTimer {
public:
    HighResolutionTimer();
    ~HighResolutionTimer();
    HighResolutionTimer(const HighResolutionTimer&) = delete;
    HighResolutionTimer& operator=(const HighResolutionTimer&) = delete;

private:
    bool active_ = false;
};

} // namespace bv
