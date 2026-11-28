#include "Player.h"

#include "audio/TimeStretch.h"

#include "core/Log.h"
#include "media/AudioDecoder.h"
#include "media/Demuxer.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <optional>
#include <vector>

namespace bv::player {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
using namespace std::chrono_literals;

double framePts(const AVFrame& f, AVRational tb) {
    const auto ts = f.best_effort_timestamp != AV_NOPTS_VALUE ? f.best_effort_timestamp : f.pts;
    return ts == AV_NOPTS_VALUE ? kNaN : static_cast<double>(ts) * av_q2d(tb);
}

} // namespace

// State shared between the demux thread (which opens the input) and the decode threads.
struct OpenedMedia {
    std::optional<media::Demuxer> demuxer;
    std::optional<media::VideoDecoder> video;
    std::optional<media::AudioDecoder> audio;
    std::mutex mutex;
    std::condition_variable cv;
    enum { Waiting, Ready, Failed } status = Waiting;
};

Player::Player(ID3D12Device* device, IVideoPresenter& presenter, AudioSinkFactory audioFactory,
               telemetry::Telemetry& telemetry)
    : device_(device), presenter_(presenter), audioFactory_(std::move(audioFactory)), telemetry_(telemetry) {}

Player::~Player() { close(); }

void Player::setState(PlayerState s) {
    std::lock_guard lock(infoMutex_);
    info_.state = s;
}

void Player::fail(const std::string& message) {
    log::error("player: {}", message);
    std::lock_guard lock(infoMutex_);
    info_.state = PlayerState::Error;
    info_.error = message;
}

void Player::open(const std::string& urlUtf8) {
    close();
    {
        std::lock_guard lock(infoMutex_);
        info_ = PlayerSnapshot{};
        info_.state = PlayerState::Opening;
    }
    videoPackets_.reset();
    audioPackets_.reset();
    frames_.reset();
    generation_ = 0;
    seekTarget_ = -1.0;
    dropBefore_ = -1.0;
    videoEnded_ = false;
    audioEnded_ = true;
    needClockReset_ = true;
    presented_ = 0;
    dropped_ = 0;
    generated_ = 0;
    clock_.reset(0.0);
    clock_.setPaused(false);
    live_ = false;
    running_ = true;

    auto media = std::make_shared<OpenedMedia>();
    if (urlUtf8 != url_) { // a new source: back to its default tracks
        url_ = urlUtf8;
        preferredAudio_ = -1;
        subtitleStream_ = -1;
    }
    {
        std::lock_guard lock(cueMutex_);
        cues_.clear();
    }
    subtitleChanged_ = true;
    const int preferredAudio = preferredAudio_;

    demuxThread_ = std::thread([this, media, url = urlUtf8, preferredAudio] {
        std::string error;
        auto demuxer = media::Demuxer::open(url, &error, preferredAudio);
        std::optional<media::VideoDecoder> video;
        std::optional<media::AudioDecoder> audio;
        if (demuxer) video = media::VideoDecoder::open(demuxer->video(), device_, static_cast<int>(4 + 2), &error);
        if (demuxer && video && demuxer->audio().index >= 0) {
            audio_ = audioFactory_ ? audioFactory_() : nullptr;
            if (audio_) {
                std::string audioError;
                audio = media::AudioDecoder::open(demuxer->audio(), audio_->sampleRate(), audio_->channels(),
                                                  &audioError);
                if (!audio) log::warn("player: audio disabled ({})", audioError);
            }
        }
        {
            std::lock_guard lock(media->mutex);
            if (demuxer && video) {
                media->demuxer = std::move(demuxer);
                media->video = std::move(video);
                media->audio = std::move(audio);
                media->status = OpenedMedia::Ready;
            } else {
                media->status = OpenedMedia::Failed;
            }
        }
        media->cv.notify_all();
        if (media->status == OpenedMedia::Failed) {
            fail(error.empty() ? "no se pudo abrir el archivo" : error);
            return;
        }

        auto& d = *media->demuxer;
        const auto* vp = d.video().params;
        {
            std::lock_guard lock(infoMutex_);
            info_.duration = d.durationSeconds();
            info_.width = vp->width;
            info_.height = vp->height;
            info_.fps = d.frameRate();
            info_.hardwareDecode = media->video->hardware();
            info_.codec = media->video->codecName();
            info_.hasAudio = media->audio.has_value();
            info_.live = d.live();
            info_.tracks = d.tracks();
            info_.audioTrack = d.audio().index;
            info_.state = PlayerState::Playing;
        }
        audioEnded_ = !media->audio.has_value();
        live_ = d.live();
        demuxLoop(*media->demuxer, media->audio.has_value());
    });

    // The decode threads pick up the opened decoders once the demux thread publishes them.
    videoThread_ = std::thread([this, media] {
        std::unique_lock lock(media->mutex);
        media->cv.wait(lock, [&] { return media->status != OpenedMedia::Waiting; });
        if (media->status != OpenedMedia::Ready) return;
        lock.unlock();

        auto& dec = *media->video;
        const AVRational tb = media->demuxer->video().timeBase;
        auto frame = media::makeFrame();
        uint64_t gen = generation_;
        auto& decodeStats = telemetry_.stage("decode");

        while (running_) {
            auto item = videoPackets_.pop();
            if (!item) break;
            if (item->flush) {
                dec.flush();
                gen = item->generation;
                continue;
            }
            if (item->generation != gen) continue;
            const auto t0 = std::chrono::steady_clock::now();
            int err = dec.send(item->packet.get()); // null packet drains
            if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF) continue;
            while ((err = dec.receive(frame.get())) >= 0) {
                decodeStats.record(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                const double pts = framePts(*frame, tb);
                const double dropBefore = dropBefore_;
                if (!std::isnan(pts) && dropBefore >= 0 && pts + 1e-3 < dropBefore) {
                    av_frame_unref(frame.get());
                    continue; // accurate seek: decoded only to reach the target
                }
                FrameItem out{media::makeFrame(), pts, gen};
                av_frame_move_ref(out.frame.get(), frame.get());
                if (!frames_.push(std::move(out))) return;
            }
            if (err == AVERROR_EOF) {
                videoEnded_ = true;
            }
        }
    });

    audioThread_ = std::thread([this, media] {
        std::unique_lock lock(media->mutex);
        media->cv.wait(lock, [&] { return media->status != OpenedMedia::Waiting; });
        if (media->status != OpenedMedia::Ready || !media->audio || !audio_) return;
        lock.unlock();

        auto& dec = *media->audio;
        std::vector<float> samples, stretched;
        uint64_t gen = generation_;
        const int channels = dec.outChannels();
        // Speed changes keep the pitch: WSOLA time-stretch between decoder and sink. Input frame
        // index -> media time is tracked so the sink still reports the right position.
        audio::TimeStretch stretch(dec.outRate(), channels);
        double stretchAnchorPts = kNaN, stretchAnchorFrame = 0, stretchPushed = 0;

        while (running_) {
            auto item = audioPackets_.pop();
            if (!item) break;
            if (item->flush) {
                dec.flush();
                audio_->flush();
                stretch.reset();
                stretchAnchorPts = kNaN;
                stretchPushed = 0;
                gen = item->generation;
                continue;
            }
            if (item->generation != gen) continue;
            dec.send(item->packet.get());
            double pts = kNaN;
            samples.clear();
            while (dec.receive(samples, pts) >= 0) {
                std::size_t offset = 0;
                const double dropBefore = dropBefore_;
                if (!std::isnan(pts) && dropBefore >= 0 && pts < dropBefore) {
                    // Trim samples that precede the seek target so audio starts in sync.
                    const auto skip = static_cast<std::size_t>((dropBefore - pts) * dec.outRate());
                    offset = std::min(skip, samples.size() / channels);
                    pts = dropBefore;
                }
                const std::size_t frames = samples.size() / channels - offset;
                for (float& s : samples) s *= volume_.load(std::memory_order_relaxed);
                const double speed = rate_.load();
                if (rateChanged_.exchange(false) || speed != stretch.rate()) {
                    stretch.reset();
                    stretch.setRate(speed);
                    stretchAnchorPts = kNaN;
                    stretchPushed = 0;
                }
                if (speed == 1.0 && std::isnan(stretchAnchorPts)) {
                    if (frames > 0 && !audio_->write(samples.data() + offset * channels, frames, pts)) return;
                } else if (frames > 0) {
                    if (std::isnan(stretchAnchorPts) && !std::isnan(pts)) {
                        stretchAnchorPts = pts;
                        stretchAnchorFrame = stretchPushed;
                    }
                    stretched.clear();
                    const double firstIn = stretch.process(samples.data() + offset * channels, frames, stretched);
                    stretchPushed += static_cast<double>(frames);
                    const double outPts = std::isnan(stretchAnchorPts)
                                              ? pts
                                              : stretchAnchorPts + (firstIn - stretchAnchorFrame) / dec.outRate();
                    const std::size_t outFrames = stretched.size() / channels;
                    if (outFrames > 0 && !audio_->write(stretched.data(), outFrames, outPts, speed)) return;
                }
                const double played = audio_->playedPosition();
                if (!std::isnan(played) && !needClockReset_) clock_.syncTo(played);
                samples.clear();
            }
            if (!item->packet) audioEnded_ = true;
        }
    });

    renderThread_ = std::thread([this] { renderLoop(); });
}

void Player::demuxLoop(media::Demuxer& demuxer, bool hasAudio) {
    auto packet = media::makePacket();
    bool eof = false;
    std::optional<media::SubtitleDecoder> subtitles;
    int subtitlesStream = -1;
    std::vector<media::SubtitleCue> fresh;
    while (running_) {
        const double target = seekTarget_.exchange(-1.0);
        if (target >= 0.0) {
            // seek() already bumped the generation and cleared the queues.
            const uint64_t gen = generation_;
            demuxer.seek(target);
            videoPackets_.push({nullptr, gen, true});
            if (hasAudio) audioPackets_.push({nullptr, gen, true});
            videoEnded_ = false;
            audioEnded_ = !hasAudio;
            eof = false;
        }
        if (eof) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        const int err = demuxer.read(packet.get());
        const uint64_t gen = generation_;
        if (err == AVERROR_EOF) {
            videoPackets_.push({nullptr, gen, false}); // drain decoders
            if (hasAudio) audioPackets_.push({nullptr, gen, false});
            eof = true;
            continue;
        }
        if (err == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        if (err < 0) {
            fail("error leyendo el archivo: " + media::errorString(err));
            return;
        }
        const bool isVideo = packet->stream_index == demuxer.video().index;
        const bool isAudio = hasAudio && packet->stream_index == demuxer.audio().index;
        if (!isVideo && !isAudio) {
            // Text subtitles: decoded right here (tiny), cues timed on the media clock.
            if (subtitleChanged_.exchange(false)) {
                subtitles.reset();
                subtitlesStream = -1;
            }
            const int wanted = subtitleStream_;
            if (wanted >= 0 && wanted != subtitlesStream) {
                const auto info = demuxer.stream(wanted);
                subtitles = media::SubtitleDecoder::open(info.params, info.timeBase);
                subtitlesStream = wanted;
            }
            if (subtitles && packet->stream_index == subtitlesStream) {
                fresh.clear();
                subtitles->decode(packet.get(), fresh);
                if (!fresh.empty()) {
                    std::lock_guard lock(cueMutex_);
                    for (auto& cue : fresh) {
                        // A cue without an end lasts until the next one starts.
                        for (auto& c : cues_)
                            if (c.openEnded && c.start < cue.start && c.end > cue.start) c.end = cue.start;
                        cues_.push_back(std::move(cue));
                    }
                    // The demuxer runs ahead of playback: keep what can still be shown.
                    const double now = clock_.now();
                    std::erase_if(cues_, [now](const media::SubtitleCue& c) { return c.end < now - 2.0; });
                }
            }
            av_packet_unref(packet.get());
            continue;
        }
        PacketItem item{media::makePacket(), gen, false};
        av_packet_move_ref(item.packet.get(), packet.get());
        if (isVideo) {
            if (!videoPackets_.push(std::move(item))) return;
        } else {
            if (!audioPackets_.push(std::move(item))) return;
        }
    }
}

void Player::renderLoop() {
    auto& presentStats = telemetry_.stage("present");
    auto& frameStats = telemetry_.stage("frame");
    std::optional<FrameItem> pending;
    auto lastPresent = std::chrono::steady_clock::time_point{};
    bool shownWhilePaused = false;
    // Frame interpolation: generated frames go between the last shown frame and `pending`
    // while waiting for it, so no latency is added. prevPts is the last real frame shown.
    double prevPts = kNaN;
    uint64_t prevGeneration = 0;
    unsigned generatedForPending = 0;

    while (running_) {
        if (!pending) {
            pending = frames_.popFor(10ms);
            if (!pending) {
                // Nothing decoded: if both streams ended, playback is over.
                if (videoEnded_ && audioEnded_ && frames_.size() == 0) {
                    std::lock_guard lock(infoMutex_);
                    if (info_.state == PlayerState::Playing) info_.state = PlayerState::Ended;
                }
                continue;
            }
            shownWhilePaused = false;
            generatedForPending = 0;
        }
        if (pending->generation != generation_) {
            pending.reset();
            continue;
        }
        if (std::isnan(pending->pts)) pending->pts = clock_.now();

        if (live_ && !clock_.paused()) {
            // Capture device: lowest latency wins. Show each frame as it arrives and skip
            // straight to the newest one if the presenter fell behind.
            if (frames_.size() > 0) {
                ++dropped_;
                pending.reset();
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            presenter_.present(*pending->frame);
            const auto t1 = std::chrono::steady_clock::now();
            presentStats.record(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (lastPresent.time_since_epoch().count() != 0)
                frameStats.record(std::chrono::duration<double, std::micro>(t1 - lastPresent).count());
            lastPresent = t1;
            ++presented_;
            needClockReset_ = true; // resume scheduling cleanly if the source stops being live
            {
                std::lock_guard lock(infoMutex_);
                info_.position = pending->pts;
            }
            pending.reset();
            continue;
        }

        if (needClockReset_) {
            clock_.reset(pending->pts);
            needClockReset_ = false;
        }

        if (clock_.paused()) {
            if (!shownWhilePaused) { // show the seek target frame while paused
                presenter_.present(*pending->frame);
                shownWhilePaused = true;
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }

        const double nextPts = frames_.withFront([](FrameItem* f) { return f ? f->pts : kNaN; });
        const double tolerance = presenter_.refreshInterval() * 0.5;
        switch (scheduleFrame(pending->pts, nextPts, clock_.now(), tolerance)) {
        case FrameAction::Wait: {
            // Generated frames at prevPts + k * dt / n. n follows the request but never exceeds
            // what the display can show (24 fps on 60 Hz -> x2, on 144 Hz up to x4).
            const unsigned factor = presenter_.interpolationFactor();
            const double dt = pending->pts - prevPts;
            if (factor > 1 && !std::isnan(prevPts) && prevGeneration == pending->generation && dt > 0 && dt <= 0.1) {
                const double refresh = std::max(presenter_.refreshInterval(), 1.0 / 500.0) * rate_.load();
                const unsigned n = std::min<unsigned>(factor, std::max(1u, static_cast<unsigned>(dt / refresh + 0.25)));
                while (generatedForPending + 1 < n) {
                    const double at = prevPts + dt * (generatedForPending + 1) / n;
                    const double now = clock_.now();
                    if (now < at - tolerance) break;
                    ++generatedForPending;
                    if (now > at + dt / n) continue; // late: skip this one rather than stutter
                    const auto t0 = std::chrono::steady_clock::now();
                    if (!presenter_.presentInterpolated(*pending->frame,
                                                        static_cast<float>(generatedForPending) / static_cast<float>(n)))
                        break;
                    const auto t1 = std::chrono::steady_clock::now();
                    presentStats.record(std::chrono::duration<double, std::micro>(t1 - t0).count());
                    if (lastPresent.time_since_epoch().count() != 0)
                        frameStats.record(std::chrono::duration<double, std::micro>(t1 - lastPresent).count());
                    lastPresent = t1;
                    ++generated_;
                }
            }
            const double wait = pending->pts - clock_.now() - tolerance;
            std::this_thread::sleep_for(std::chrono::duration<double>(std::clamp(wait, 0.0005, 0.004)));
            break;
        }
        case FrameAction::Drop:
            ++dropped_;
            pending.reset();
            break;
        case FrameAction::Present: {
            const auto t0 = std::chrono::steady_clock::now();
            presenter_.present(*pending->frame);
            const auto t1 = std::chrono::steady_clock::now();
            presentStats.record(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (lastPresent.time_since_epoch().count() != 0) {
                frameStats.record(std::chrono::duration<double, std::micro>(t1 - lastPresent).count());
            }
            lastPresent = t1;
            ++presented_;
            prevPts = pending->pts;
            prevGeneration = pending->generation;
            {
                std::lock_guard lock(infoMutex_);
                info_.position = pending->pts;
            }
            pending.reset();
            break;
        }
        }
    }
}

void Player::setRate(double rate) {
    rate = std::clamp(rate, 0.25, 4.0);
    if (live_) rate = 1.0;
    if (rate == rate_.load()) return;
    rate_ = rate;
    rateChanged_ = true;
    clock_.setRate(rate);
}

void Player::play() {
    clock_.setPaused(false);
    if (audio_) audio_->setPaused(false);
    std::lock_guard lock(infoMutex_);
    if (info_.state == PlayerState::Paused) info_.state = PlayerState::Playing;
}

void Player::pause() {
    clock_.setPaused(true);
    if (audio_) audio_->setPaused(true);
    std::lock_guard lock(infoMutex_);
    if (info_.state == PlayerState::Playing) info_.state = PlayerState::Paused;
}

void Player::seek(double seconds) {
    if (live_) return; // nothing to seek in a capture device
    seconds = std::max(0.0, seconds);
    // Invalidate everything in flight first so blocked producers wake up immediately,
    // even while paused (the render thread is not consuming then).
    ++generation_;
    dropBefore_ = seconds;
    needClockReset_ = true;
    {
        std::lock_guard lock(cueMutex_);
        cues_.clear();
    }
    subtitleChanged_ = true; // the decoder restarts after the seek
    videoPackets_.clear();
    audioPackets_.clear();
    frames_.clear();
    seekTarget_ = seconds;
    std::lock_guard lock(infoMutex_);
    info_.position = seconds;
    if (info_.state == PlayerState::Ended) info_.state = clock_.paused() ? PlayerState::Paused : PlayerState::Playing;
}

void Player::setVolume(float volume) { volume_ = std::clamp(volume, 0.0f, 2.0f); }

void Player::selectAudioTrack(int streamIndex) {
    if (live_ || url_.empty()) return;
    const auto s = snapshot();
    if (streamIndex == s.audioTrack) return;
    const double position = s.position;
    const bool paused = clock_.paused();
    const std::string url = url_;
    const int subtitle = subtitleStream_;
    preferredAudio_ = streamIndex;
    open(url); // same URL: keeps preferredAudio_
    subtitleStream_ = subtitle;
    if (position > 0.5) seek(position);
    if (paused) pause();
}

void Player::selectSubtitleTrack(int streamIndex) {
    subtitleStream_ = streamIndex;
    subtitleChanged_ = true;
    std::lock_guard lock(cueMutex_);
    cues_.clear();
    // Cues of the new track arrive as the demuxer reads on; already-read ones are not replayed.
}

std::string Player::subtitleNow() const {
    const double t = clock_.now();
    std::lock_guard lock(cueMutex_);
    std::string out;
    for (const auto& c : cues_) {
        if (c.start <= t && t < c.end) {
            if (!out.empty()) out += '\n';
            out += c.text;
        }
    }
    return out;
}

void Player::stopThreads() {
    running_ = false;
    videoPackets_.abort();
    audioPackets_.abort();
    frames_.abort();
    if (audio_) audio_->stop();
    for (auto* t : {&demuxThread_, &videoThread_, &audioThread_, &renderThread_}) {
        if (t->joinable()) t->join();
    }
    audio_.reset();
}

void Player::close() {
    stopThreads();
    std::lock_guard lock(infoMutex_);
    if (info_.state != PlayerState::Error) info_.state = PlayerState::Idle;
}

PlayerSnapshot Player::snapshot() const {
    std::lock_guard lock(infoMutex_);
    PlayerSnapshot s = info_;
    s.subtitleTrack = subtitleStream_;
    s.framesPresented = presented_;
    s.framesDropped = dropped_;
    s.framesGenerated = generated_;
    return s;
}

} // namespace bv::player
