#pragma once

#include <string>
#include <vector>

namespace bv::media {

struct CaptureDevice {
    std::string name;      // friendly name, what DirectShow / capture: URLs use
    std::string id;        // symbolic link (stable across reboots), informative
    bool audio = false;
};

// Video and audio capture devices (capture cards, webcams, line-in). Windows: Media Foundation.
std::vector<CaptureDevice> enumerateCaptureDevices();

} // namespace bv::media
