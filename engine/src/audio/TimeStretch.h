#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bv::audio {

// Changes playback speed without changing pitch (WSOLA: overlap-add of 20 ms Hann-windowed
// segments, each picked within +-10 ms of its nominal place where it best continues the previous
// one). Interleaved float input of any channel count. rate 1 passes samples through untouched.
class TimeStretch {
public:
    TimeStretch(int sampleRate, int channels);

    void setRate(double rate); // 0.25 .. 4
    [[nodiscard]] double rate() const { return rate_; }
    void reset();

    // Appends output to `out` (interleaved). Returns the absolute input frame index (frames
    // pushed since the last reset) that the first appended output frame corresponds to.
    double process(const float* in, std::size_t frames, std::vector<float>& out);

private:
    void emitHop(std::vector<float>& out, std::size_t start);
    std::size_t bestOffset(std::size_t nominal) const;

    int rate_hz_;
    int channels_;
    double rate_ = 1.0;
    std::size_t hop_;    // output hop (half a segment)
    std::size_t delta_;  // search radius
    std::vector<float> window_;
    std::vector<float> in_;      // buffered input (interleaved), starting at absolute frame base_
    std::vector<float> tail_;    // second half of the last segment, waiting for overlap
    double base_ = 0;            // absolute index of in_[0]
    double position_ = 0;        // absolute nominal start of the next segment
    double prevStart_ = -1;      // absolute start of the last chosen segment
    double pushed_ = 0;          // frames pushed since reset
};

} // namespace bv::audio
