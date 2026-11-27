#pragma once

#include "player/Sinks.h"

#include <memory>

namespace bv::audio {

// Shared-mode WASAPI output on the default render device, float32 at the mix rate.
std::unique_ptr<player::IAudioSink> createWasapiSink();

} // namespace bv::audio
