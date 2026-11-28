#pragma once

#include "BoundedQueue.h"
#include "MediaClock.h"
#include "Sinks.h"
#include "media/Demuxer.h"
#include "media/SubtitleDecoder.h"
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
    bool live = false; // capture device: shown as it arrives, no seeking
    std::string codec;
    std::string error;
    uint64_t framesPresented = 0;
    uint64_t framesDropped = 0;
    uint64_t framesGenerated = 0; // interpolated frames shown
    std::vector<media::Track> tracks;
    int audioTrack = -1, subtitleTrack = -1; // stream indices, -1 none
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
    // Audio: reopens at the current position with that stream. Subtitles: -1 = off.
    void selectAudioTrack(int streamIndex);
    void selectSubtitleTrack(int streamIndex);
    // Subtitle text showing now (UTF-8, '\n' between lines), empty when none.
    [[nodiscard]] std::string subtitleNow() const;
    // Playback speed 0.25..4 (files only; live sources always run at 1). Pitch is kept.
    void setRate(double rate);
    [[nodiscard]] double rate() const { return rate_.load(); }

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
    std::atomic<bool> live_{false};
    std::atomic<uint64_t> presented_{0}, dropped_{0}, generated_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<double> rate_{1.0};
    // Subtitles: decoded on the demux thread into cues, read by the UI.
    std::atomic<int> subtitleStream_{-1};
    std::atomic<bool> subtitleChanged_{false};
    mutable std::mutex cueMutex_;
    std::vector<media::SubtitleCue> cues_;
    std::string url_;
    int preferredAudio_ = -1;
    std::atomic<bool> rateChanged_{false};

    mutable std::mutex infoMutex_;
    PlayerSnapshot info_;
};

} // namespace bv::player
