#pragma once

#include "BoundedQueue.h"
#include "MediaClock.h"
#include "Sinks.h"
#include "core/TimerResolution.h"
#include "media/Demuxer.h"
#include "media/FFmpeg.h"
#include "telemetry/Telemetry.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct ID3D12Device;

namespace bv::player {

enum class PlayerState { Idle, Opening, Playing, Paused, Ended, Error };

struct PlayerSnapshot {
    PlayerState state = PlayerState::Idle;
    double position = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hardwareDecode = false;
    bool hasAudio = false;
    std::string codec;
    std::string error;
    uint64_t framesPresented = 0;
    uint64_t framesDropped = 0;
};

// Plays one media source: demux -> decode (GPU when possible) -> schedule -> present,
// with audio as the master clock. Four threads, all bounded queues, no unbounded buffering.
class Player {
public:
    using AudioSinkFactory = std::function<std::unique_ptr<IAudioSink>()>;

    Player(ID3D12Device* device, IVideoPresenter& presenter, AudioSinkFactory audioFactory,
           telemetry::Telemetry& telemetry);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Opens asynchronously; poll snapshot() for Playing/Error. Starts playing when ready.
    void open(const std::string& urlUtf8);
    void play();
    void pause();
    void seek(double seconds);
    void close();
    void setVolume(float volume);

    [[nodiscard]] PlayerSnapshot snapshot() const;

private:
    struct PacketItem {
        media::PacketPtr packet; // null = end of stream (drain) unless flush
        uint64_t generation = 0;
        bool flush = false;
    };
    struct FrameItem {
        media::FramePtr frame;
        double pts = 0.0;
        uint64_t generation = 0;
    };

    void demuxLoop(media::Demuxer& demuxer, bool hasAudio);
    void renderLoop();
    void fail(const std::string& message);
    void setState(PlayerState s);
    void stopThreads();

    ID3D12Device* device_;
    IVideoPresenter& presenter_;
    AudioSinkFactory audioFactory_;
    HighResolutionTimer timer_; // precise frame pacing while a player exists
    telemetry::Telemetry& telemetry_;

    BoundedQueue<PacketItem> videoPackets_{96};
    BoundedQueue<PacketItem> audioPackets_{192};
    BoundedQueue<FrameItem> frames_{4};
    MediaClock clock_;

    std::unique_ptr<IAudioSink> audio_;
    std::thread demuxThread_, videoThread_, audioThread_, renderThread_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<double> seekTarget_{-1.0};   // pending seek for the demux thread
    std::atomic<double> dropBefore_{-1.0};   // accurate seek: discard frames before this pts
    std::atomic<bool> videoEnded_{false};
    std::atomic<bool> audioEnded_{true};
    std::atomic<bool> needClockReset_{true};
    std::atomic<uint64_t> presented_{0}, dropped_{0};
    std::atomic<float> volume_{1.0f};

    mutable std::mutex infoMutex_;
    PlayerSnapshot info_;
};

} // namespace bv::player
