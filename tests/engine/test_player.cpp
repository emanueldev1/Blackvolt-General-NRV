#include "check.h"
#include "player/BoundedQueue.h"
#include "player/MediaClock.h"
#include "player/Player.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

using namespace bv::player;
using namespace std::chrono_literals;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class RecordingPresenter final : public IVideoPresenter {
public:
    bool present(const AVFrame& frame) override {
        std::lock_guard lock(mutex);
        widths.push_back(frame.width);
        return true;
    }
    std::size_t count() {
        std::lock_guard lock(mutex);
        return widths.size();
    }
    std::mutex mutex;
    std::vector<int> widths;
};

// Consumes audio in real time, like a device would.
class RealtimeSink final : public IAudioSink {
public:
    int sampleRate() const override { return 48000; }
    int channels() const override { return 2; }
    bool write(const float*, std::size_t frames, double pts, double) override {
        if (stopped) return false;
        std::this_thread::sleep_for(std::chrono::duration<double>(double(frames) / 48000.0));
        position = pts + double(frames) / 48000.0;
        total += frames;
        return !stopped;
    }
    double playedPosition() const override { return position; }
    void setPaused(bool) override {}
    void flush() override { ++flushes; }
    void setVolume(float) override {}
    void stop() override { stopped = true; }
    std::atomic<double> position{kNaN};
    std::atomic<std::size_t> total{0};
    std::atomic<int> flushes{0};
    std::atomic<bool> stopped{false};
};

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

const std::string kClip = std::string(BV_SOURCE_DIR) + "/tests/data/clip_2s.mp4";

} // namespace

TEST(schedule_frame_decisions) {
    const double tol = 0.008;
    CHECK(scheduleFrame(1.000, 1.033, 0.950, tol) == FrameAction::Wait);
    CHECK(scheduleFrame(1.000, 1.033, 0.995, tol) == FrameAction::Present);
    CHECK(scheduleFrame(1.000, 1.033, 1.030, tol) == FrameAction::Drop);  // next is due too
    CHECK(scheduleFrame(1.000, kNaN, 1.500, tol) == FrameAction::Present); // last frame: show it
}

TEST(media_clock_pause_and_reset) {
    MediaClock c;
    c.reset(5.0);
    CHECK(c.paused());
    std::this_thread::sleep_for(20ms);
    CHECK_NEAR(c.now(), 5.0, 1e-9);
    c.setPaused(false);
    std::this_thread::sleep_for(50ms);
    CHECK(c.now() > 5.03 && c.now() < 5.2);
    c.setPaused(true);
    const double frozen = c.now();
    std::this_thread::sleep_for(20ms);
    CHECK_NEAR(c.now(), frozen, 1e-9);
}

TEST(bounded_queue_abort_unblocks) {
    BoundedQueue<int> q(1);
    CHECK(q.push(1));
    std::thread t([&] { std::this_thread::sleep_for(20ms); q.abort(); });
    CHECK(!q.push(2)); // blocked on full, released by abort
    t.join();
    CHECK(!q.pop().has_value());
}

TEST(player_plays_clip_in_real_time) {
    bv::telemetry::Telemetry telemetry;
    RecordingPresenter presenter;
    RealtimeSink* sinkRaw = nullptr;
    Player player(nullptr, presenter, [&] {
        auto s = std::make_unique<RealtimeSink>();
        sinkRaw = s.get();
        return s;
    }, telemetry);

    const auto start = std::chrono::steady_clock::now();
    player.open(kClip);
    CHECK(waitFor([&] { return player.snapshot().state == PlayerState::Playing; }, 3000ms));
    const auto info = player.snapshot();
    CHECK(info.width == 320 && info.height == 180);
    CHECK(info.hasAudio);
    CHECK(info.codec == "h264");
    CHECK(std::fabs(info.fps - 30.0) < 0.01);

    CHECK(waitFor([&] { return player.snapshot().state == PlayerState::Ended; }, 6000ms));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto end = player.snapshot();
    // 60 frames in ~2 s of wall time: real-time pacing, nearly nothing dropped.
    CHECK(end.framesPresented + end.framesDropped >= 58);
    CHECK(end.framesDropped <= 3);
    CHECK(elapsed > 1.8 && elapsed < 3.5);
    CHECK(telemetry.find("frame") && telemetry.find("frame")->snapshot().samples > 50);
    const auto frameStats = telemetry.find("frame")->snapshot();
    CHECK(frameStats.avg > 25000 && frameStats.avg < 42000); // ~33.3 ms per frame
    CHECK(sinkRaw && sinkRaw->total > 48000 * 3 / 2);
    player.close();
}

TEST(player_seek_while_paused_shows_target) {
    bv::telemetry::Telemetry telemetry;
    RecordingPresenter presenter;
    Player player(nullptr, presenter, [] { return std::make_unique<RealtimeSink>(); }, telemetry);
    player.open(kClip);
    CHECK(waitFor([&] { return player.snapshot().state == PlayerState::Playing; }, 3000ms));
    player.pause();
    std::this_thread::sleep_for(100ms);
    const auto before = presenter.count();
    player.seek(1.5);
    CHECK(waitFor([&] { return presenter.count() > before; }, 2000ms)); // target frame shown
    CHECK(std::fabs(player.snapshot().position - 1.5) < 0.05);
    player.play();
    CHECK(waitFor([&] { return player.snapshot().state == PlayerState::Ended; }, 3000ms));
    player.close();
}

TEST(player_reports_open_errors) {
    bv::telemetry::Telemetry telemetry;
    RecordingPresenter presenter;
    Player player(nullptr, presenter, nullptr, telemetry);
    player.open("/definitely/not/here.mp4");
    CHECK(waitFor([&] { return player.snapshot().state == PlayerState::Error; }, 2000ms));
    CHECK(!player.snapshot().error.empty());
}

#include "render/ColorMatrix.h"

TEST(yuv_matrix_limited_range_extremes) {
    using namespace bv::render;
    const auto m = yuvToRgb(YuvMatrix::Bt709, false);
    auto apply = [&](float y, float u, float v, int row) {
        return m[row * 4] * y + m[row * 4 + 1] * u + m[row * 4 + 2] * v + m[row * 4 + 3];
    };
    const float black = 16.f / 255, white = 235.f / 255, mid = 128.f / 255;
    for (int r = 0; r < 3; ++r) {
        CHECK_NEAR(apply(black, mid, mid, r), 0.0f, 1e-4);
        CHECK_NEAR(apply(white, mid, mid, r), 1.0f, 1e-4);
    }
    // Pure BT.709 red (Y=63, Cb=102, Cr=240 in 8-bit limited range).
    CHECK_NEAR(apply(63.f / 255, 102.f / 255, 240.f / 255, 0), 1.0f, 0.01);
    CHECK_NEAR(apply(63.f / 255, 102.f / 255, 240.f / 255, 1), 0.0f, 0.01);
}

TEST(fit_aspect_letterbox_and_pillarbox) {
    using namespace bv::render;
    auto v = fitAspect(1920, 1080, 1, 1000, 1000); // 16:9 in a square -> letterbox
    CHECK_NEAR(v.width, 1000, 0.01);
    CHECK_NEAR(v.height, 562.5, 0.01);
    CHECK_NEAR(v.y, 218.75, 0.01);
    v = fitAspect(1440, 1080, 1, 1920, 1080); // 4:3 in 16:9 -> pillarbox
    CHECK_NEAR(v.width, 1440, 0.01);
    CHECK_NEAR(v.x, 240, 0.01);
}
