#pragma once

#include "Demuxer.h"
#include "FFmpeg.h"

#include <optional>
#include <string>
#include <vector>

namespace bv::media {

// Decodes audio and converts it to interleaved float32 at the output device's
// rate and channel count (what WASAPI shared mode wants).
class AudioDecoder {
public:
    static std::optional<AudioDecoder> open(const StreamInfo& stream, int outRate, int outChannels,
                                            std::string* error);

    AudioDecoder(AudioDecoder&&) noexcept = default;
    AudioDecoder& operator=(AudioDecoder&&) noexcept = default;

    int send(const AVPacket* packet) { return avcodec_send_packet(ctx_.get(), packet); }
    // Receives one decoded frame and appends converted samples to `out`.
    // Returns the frame's start time in seconds via `pts` (NaN if unknown).
    int receive(std::vector<float>& out, double& pts);
    void flush();

    [[nodiscard]] int outRate() const { return outRate_; }
    [[nodiscard]] int outChannels() const { return outChannels_; }

private:
    AudioDecoder() = default;
    CodecCtxPtr ctx_;
    SwrPtr swr_;
    FramePtr frame_;
    AVRational timeBase_{0, 1};
    int outRate_ = 48000;
    int outChannels_ = 2;
};

} // namespace bv::media
