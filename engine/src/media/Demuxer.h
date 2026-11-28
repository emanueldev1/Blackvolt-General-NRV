#pragma once

#include "FFmpeg.h"

#include <optional>
#include <string>
#include <vector>

namespace bv::media {

struct StreamInfo {
    int index = -1;
    AVRational timeBase{0, 1};
    const AVCodecParameters* params = nullptr;
};

// An audio or text-subtitle stream the user can choose.
struct Track {
    int index = -1;
    int kind = 0; // 0 audio, 1 subtitle
    std::string language, title, codec;
};

// Opens a file or URL and reads packets. Not thread-safe: owned by the demux thread.
class Demuxer {
public:
    // Returns nullopt and fills `error` when the input cannot be opened or has no video.
    // preferredAudio: stream index of the audio track to play, -1 = best.
    static std::optional<Demuxer> open(const std::string& urlUtf8, std::string* error, int preferredAudio = -1);

    Demuxer(Demuxer&&) noexcept = default;
    Demuxer& operator=(Demuxer&&) noexcept = default;

    [[nodiscard]] const StreamInfo& video() const { return video_; }
    [[nodiscard]] const StreamInfo& audio() const { return audio_; } // index -1 if none
    [[nodiscard]] const std::vector<Track>& tracks() const { return tracks_; }
    [[nodiscard]] StreamInfo stream(int index) const;
    [[nodiscard]] double durationSeconds() const;
    [[nodiscard]] double frameRate() const;
    [[nodiscard]] AVFormatContext* context() const { return fmt_.get(); }
    // A capture device (see CaptureUrl.h): no duration, no seeking, frames must not queue up.
    [[nodiscard]] bool live() const { return live_; }

    // 0 on success, AVERROR_EOF at end of input, another negative AVERROR on failure.
    int read(AVPacket* packet);
    // Seeks to the keyframe at or before `seconds`.
    bool seek(double seconds);

private:
    Demuxer() = default;
    FormatCtxPtr fmt_;
    StreamInfo video_;
    StreamInfo audio_;
    std::vector<Track> tracks_;
    bool live_ = false;
};

} // namespace bv::media
