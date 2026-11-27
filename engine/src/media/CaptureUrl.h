#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bv::media {

// Live capture inputs are opened through the same player as files, with a URL of the form
//
//     capture:video=<device name>[:audio=<device name>][?size=WxH&fps=N&format=mjpeg]
//
// which maps to FFmpeg's DirectShow input on Windows (v4l2 elsewhere, video only).
struct CaptureUrl {
    std::string inputFormat;                                   // "dshow" / "v4l2"
    std::string target;                                        // what avformat_open_input receives
    std::vector<std::pair<std::string, std::string>> options;  // demuxer options
};

[[nodiscard]] bool isCaptureUrl(std::string_view url);
[[nodiscard]] std::optional<CaptureUrl> parseCaptureUrl(std::string_view url, std::string* error = nullptr);

// Builds the URL from its parts (the inverse of parseCaptureUrl, used by the app and tests).
[[nodiscard]] std::string makeCaptureUrl(std::string_view video, std::string_view audio, int width, int height, int fps);

} // namespace bv::media
