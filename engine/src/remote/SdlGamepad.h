#pragma once

#include "StreamSession.h"

#include <memory>

namespace bv::remote {

// Every pad SDL3 knows (DualSense, DualShock 4, Xbox, Switch, generic): buttons, sticks,
// touchpad, gyro/accel with orientation, rumble, light bar and DualSense adaptive triggers.
// Returns nullptr when built without SDL3.
std::unique_ptr<IInputDevice> createSdlGamepad();

} // namespace bv::remote
