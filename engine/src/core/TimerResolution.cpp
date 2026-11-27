#include "TimerResolution.h"

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

namespace bv {

#if defined(_WIN32)
HighResolutionTimer::HighResolutionTimer() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
HighResolutionTimer::~HighResolutionTimer() {
    if (active_) timeEndPeriod(1);
}
#else
HighResolutionTimer::HighResolutionTimer() = default;
HighResolutionTimer::~HighResolutionTimer() { (void)active_; }
#endif

} // namespace bv
