#pragma once

// Offline transcoder: decode → scale (Lanczos) → hardware encoder when present (NVENC, then
// Media Foundation), software fallback otherwise. Audio is copied untouched when the container
// accepts it. Runs on its own thread; progress is polled.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bv::exporter {

enum class Codec { H264 = 0, Hevc = 1, Av1 = 2 };

// Per-frame processing between decode and encode (DLSS NR on the GPU). Frames are RGBA8 at the
// source size, sRGB-encoded, in display order.
class FrameFilter {
public:
    virtual ~FrameFilter() = default;
    virtual bool process(const uint8_t* rgba, int stride, uint8_t* out, int outStride) = 0;
    [[nodiscard]] virtual std::string describe() const { return {}; }
};
// Creates the filter once the source size and frame interval are known; null + error on failure.
using FilterFactory = std::function<std::unique_ptr<FrameFilter>(uint32_t width, uint32_t height, double frameMs,
                                                                  std::string* error)>;

struct Options {
    std::string input;
    std::string output;          // container chosen from the extension (.mp4, .mkv, .mov)
    Codec codec = Codec::Hevc;
    uint32_t width = 0;          // 0 = keep; when only one is set the other follows the aspect
    uint32_t height = 0;
    uint32_t quality = 70;       // 0..100, higher = better
    bool audio = true;
    std::vector<std::string> encoders; // empty = default list for the codec
    FilterFactory filter;              // optional (DLSS NR)
};

enum class State { Idle = 0, Running = 1, Done = 2, Failed = 3, Cancelled = 4 };

struct Progress {
    State state = State::Idle;
    double fraction = 0.0;       // 0..1
    double fps = 0.0;            // encoded frames per second
    uint64_t frames = 0;
    double elapsedS = 0.0;
    double etaS = 0.0;
    bool hardware = false;
    uint32_t outWidth = 0, outHeight = 0;
    std::string encoder;
    std::string enhance;         // e.g. "DLSS NR Compatible SF 310.9.16.0"
    std::string message;         // error, or a note (e.g. audio dropped)
};

// Encoder names tried in order for a codec.
std::vector<std::string> defaultEncoders(Codec codec);
// Even output size that keeps the source aspect ratio.
void fitSize(uint32_t srcW, uint32_t srcH, uint32_t reqW, uint32_t reqH, uint32_t& outW, uint32_t& outH);

class Job {
public:
    Job() = default;
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    void start(Options options);
    void cancel() { cancel_ = true; }
    void wait();
    [[nodiscard]] Progress progress() const;

private:
    void run();
    void update(const std::function<void(Progress&)>& fn);

    Options options_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    Progress progress_;
};

} // namespace bv::exporter
