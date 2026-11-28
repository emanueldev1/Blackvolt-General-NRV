#pragma once

#include "media/FFmpeg.h"

#include <cstddef>

namespace bv::player {

// Receives decoded frames at their presentation time. Implementations: D3D12 swapchain
// presenter (Windows) and a recording presenter used by tests.
class IVideoPresenter {
public:
    virtual ~IVideoPresenter() = default;
    // `frame` is either a software frame or an AV_PIX_FMT_D3D12 hardware frame.
    virtual bool present(const AVFrame& frame) = 0;
    // Redraw the last frame (after resize / while paused). Optional.
    virtual void redraw() {}
    // Display refresh interval in seconds, used as the scheduling tolerance.
    [[nodiscard]] virtual double refreshInterval() const { return 1.0 / 60.0; }
    // Frame interpolation: how many frames to show per source frame (1 = off).
    [[nodiscard]] virtual unsigned interpolationFactor() const { return 1; }
    // Shows a frame between the last presented one and `next` (not shown yet) at 0 < t < 1.
    // Returns false when it cannot (no previous frame, size change): the caller just waits.
    virtual bool presentInterpolated(const AVFrame& next, float t) {
        (void)next;
        (void)t;
        return false;
    }
};

// Plays interleaved float32 audio. write() blocks until the device has room, which is
// what paces the audio thread.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    [[nodiscard]] virtual int sampleRate() const = 0;
    [[nodiscard]] virtual int channels() const = 0;
    // `pts` is the timestamp of the first sample; each written sample advances media time by
    // `speed` / sampleRate (time-stretched playback). Returns false if the sink was stopped.
    virtual bool write(const float* samples, std::size_t frames, double pts, double speed = 1.0) = 0;
    // Timestamp of the sample currently audible, or NaN if unknown.
    [[nodiscard]] virtual double playedPosition() const = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void flush() = 0;
    virtual void setVolume(float volume) = 0;
    virtual void stop() = 0; // unblocks write()
};

} // namespace bv::player
