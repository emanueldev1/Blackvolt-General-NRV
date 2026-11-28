#include "StreamSession.h"

#include "ChiakiRuntime.h"
#include "Discovery.h"
#include "core/Log.h"
#include "core/TimerResolution.h"
#include "media/FFmpeg.h"
#include "media/VideoDecoder.h"
#include "player/BoundedQueue.h"

#include <chiaki/controller.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/session.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace bv::remote {
namespace {

using Clock = std::chrono::steady_clock;

struct VideoSample {
    media::PacketPtr packet;
    Clock::time_point arrived;
};

ChiakiControllerState toChiaki(const GamepadState& g) {
    ChiakiControllerState c;
    chiaki_controller_state_set_idle(&c);
    c.buttons = g.buttons;
    c.l2_state = g.l2;
    c.r2_state = g.r2;
    c.left_x = g.leftX;
    c.left_y = g.leftY;
    c.right_x = g.rightX;
    c.right_y = g.rightY;
    for (int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i) {
        c.touches[i].id = g.touches[i].id;
        c.touches[i].x = g.touches[i].x;
        c.touches[i].y = g.touches[i].y;
    }
    c.gyro_x = g.gyro[0];
    c.gyro_y = g.gyro[1];
    c.gyro_z = g.gyro[2];
    c.accel_x = g.accel[0];
    c.accel_y = g.accel[1];
    c.accel_z = g.accel[2];
    c.orient_x = g.orient[0];
    c.orient_y = g.orient[1];
    c.orient_z = g.orient[2];
    c.orient_w = g.orient[3];
    return c;
}

// Sticks: take whichever source is pushed further; buttons and triggers: union.
GamepadState merge(const GamepadState& a, const GamepadState& b) {
    GamepadState m = a;
    m.buttons |= b.buttons;
    m.l2 = std::max(a.l2, b.l2);
    m.r2 = std::max(a.r2, b.r2);
    auto pick = [](int16_t x, int16_t y) { return std::abs(int(y)) > std::abs(int(x)) ? y : x; };
    m.leftX = pick(a.leftX, b.leftX);
    m.leftY = pick(a.leftY, b.leftY);
    m.rightX = pick(a.rightX, b.rightX);
    m.rightY = pick(a.rightY, b.rightY);
    return m;
}

bool shouldRetry(ChiakiQuitReason reason) {
    switch (reason) {
    case CHIAKI_QUIT_REASON_STOPPED:
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN: // console turned off
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
    case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
        return false;
    default:
        return true;
    }
}

std::string quitText(ChiakiQuitReason reason, const char* detail) {
    std::string s;
    switch (reason) {
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE: s = "Remote Play ya está en uso en otro dispositivo"; break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH: s = "Remote Play falló en la consola"; break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH: s = "versión de Remote Play incompatible"; break;
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
    case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED: s = "la consola rechazó la conexión"; break;
    case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED: s = "no se pudo conectar con la consola"; break;
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED: s = "la consola cerró la conexión"; break;
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN: s = "la consola se apagó"; break;
    case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED: s = "registro PSN fallido"; break;
    default: s = chiaki_quit_reason_string(reason); break;
    }
    if (detail && *detail) s += std::string(" (") + detail + ")";
    return s;
}

} // namespace

struct StreamSession::Impl {
    Impl(ID3D12Device* d, player::IVideoPresenter& p, AudioSinkFactory a, std::unique_ptr<IInputDevice> i,
         telemetry::Telemetry& t)
        : device(d), presenter(p), audioFactory(std::move(a)), input(std::move(i)), telemetry(t),
          decodeStage(t.stage("stream.decode")), latencyStage(t.stage("stream.latency")) {}

    ID3D12Device* device;
    player::IVideoPresenter& presenter;
    AudioSinkFactory audioFactory;
    std::unique_ptr<IInputDevice> input;
    telemetry::Telemetry& telemetry;
    telemetry::StageStats& decodeStage;
    telemetry::StageStats& latencyStage;
    HighResolutionTimer timer; // 250 Hz input polling needs 1 ms sleeps

    ConsoleProfile console;
    StreamSettings settings;

    // Session (owned by the control thread; others touch it only under sessionMutex while live).
    ChiakiSession session{};
    ChiakiOpusDecoder opus{};
    std::mutex sessionMutex;
    bool sessionLive = false;

    // Control thread signalling.
    std::thread control, video, audioThread, inputThread;
    std::mutex controlMutex;
    std::condition_variable controlCv;
    bool stopRequested = false;
    bool quitReceived = false;
    ChiakiQuitReason quitReason = CHIAKI_QUIT_REASON_NONE;
    std::string quitDetail;

    // Media.
    player::BoundedQueue<VideoSample> samples{6};
    const Clock::time_point epoch = Clock::now();
    std::atomic<bool> keyframeWanted{false};
    player::BoundedQueue<std::vector<float>> pcm{12};
    std::unique_ptr<player::IAudioSink> audioSink;
    media::SwrPtr swr;
    int pcmRate = 0, pcmChannels = 0;
    std::atomic<ChiakiCodec> codec{CHIAKI_CODEC_H264};
    std::atomic<bool> resetDecoder{true};
    std::atomic<float> volume{1.0f};

    // Input.
    std::mutex overlayMutex;
    GamepadState overlay;
    std::atomic<int64_t> psUntil{0}; // steady_clock ticks; PS held until then

    // Stats.
    std::atomic<uint64_t> bytesReceived{0}, framesPresented{0}, framesLost{0}, framesRecovered{0};
    std::atomic<uint32_t> reconnects{0};
    mutable std::mutex infoMutex;
    StreamSnapshot info;

    void setState(StreamState s, const std::string& err = {}) {
        std::lock_guard lock(infoMutex);
        info.state = s;
        if (!err.empty()) info.error = err;
    }

    // ---- chiaki callbacks (protocol threads: never block here) ----
    static void onEvent(ChiakiEvent* e, void* user) {
        auto* self = static_cast<Impl*>(user);
        switch (e->type) {
        case CHIAKI_EVENT_CONNECTED: {
            self->codec = self->session.connect_info.video_profile.codec;
            self->resetDecoder = true;
            {
                std::lock_guard lock(self->infoMutex);
                self->info.state = StreamState::Streaming;
                self->info.error.clear();
                self->info.width = self->session.connect_info.video_profile.width;
                self->info.height = self->session.connect_info.video_profile.height;
                self->info.rttMs = static_cast<double>(self->session.rtt_us) / 1000.0;
                self->info.codec = chiaki_codec_name(self->codec);
            }
            log::info("remoteplay: streaming {}x{}@{} {} ({} kbps)", self->session.connect_info.video_profile.width,
                      self->session.connect_info.video_profile.height,
                      self->session.connect_info.video_profile.max_fps, chiaki_codec_name(self->codec),
                      self->session.connect_info.video_profile.bitrate);
            break;
        }
        case CHIAKI_EVENT_QUIT: {
            std::lock_guard lock(self->controlMutex);
            self->quitReceived = true;
            self->quitReason = e->quit.reason;
            self->quitDetail = e->quit.reason_str ? e->quit.reason_str : "";
            self->controlCv.notify_all();
            break;
        }
        case CHIAKI_EVENT_RUMBLE:
            if (self->input) self->input->rumble(e->rumble.left, e->rumble.right);
            break;
        case CHIAKI_EVENT_LED_COLOR:
            if (self->input) self->input->setLed(e->led_state[0], e->led_state[1], e->led_state[2]);
            break;
        case CHIAKI_EVENT_TRIGGER_EFFECTS:
            if (self->input && self->settings.enableDualSense) {
                self->input->setTriggerEffects(e->trigger_effects.type_left, e->trigger_effects.left,
                                               e->trigger_effects.type_right, e->trigger_effects.right);
            }
            break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST: {
            std::lock_guard lock(self->controlMutex);
            self->quitReceived = true;
            self->quitReason = CHIAKI_QUIT_REASON_STOPPED;
            self->quitDetail = "la consola pide el PIN de inicio de sesión de Remote Play";
            self->controlCv.notify_all();
            break;
        }
        default:
            break;
        }
    }

    static bool onVideo(uint8_t* buf, size_t size, int32_t lost, bool recovered, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->bytesReceived += size;
        if (lost > 0) self->framesLost += static_cast<uint64_t>(lost);
        if (recovered) ++self->framesRecovered;
        auto pkt = media::makePacket();
        if (!pkt || av_new_packet(pkt.get(), static_cast<int>(size)) < 0) return false;
        std::memcpy(pkt->data, buf, size);
        const auto arrived = Clock::now();
        // Arrival time as the timestamp (decoder time base is 1/1000000): temporal stages such as
        // DLSS NR and anti-flicker need increasing timestamps, or every frame looks like a cut.
        pkt->pts = pkt->dts = std::chrono::duration_cast<std::chrono::microseconds>(arrived - self->epoch).count();
        VideoSample s{std::move(pkt), arrived};
        if (self->samples.size() >= 6) {
            // Decoder can't keep up (e.g. while DLSS NR builds its model): drop the backlog, restart
            // the decoder and ask the console for a keyframe, or the picture stays frozen until the
            // console sends one on its own.
            self->samples.clear();
            self->resetDecoder = true;
            self->keyframeWanted = true;
            return false;
        }
        return self->samples.push(std::move(s));
    }

    static void onAudioSettings(uint32_t channels, uint32_t rate, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->pcmChannels = static_cast<int>(channels);
        self->pcmRate = static_cast<int>(rate);
        self->swr.reset();
        if (!self->audioSink || channels == 0 || rate == 0) return;
        AVChannelLayout in{}, out{};
        av_channel_layout_default(&in, static_cast<int>(channels));
        av_channel_layout_default(&out, self->audioSink->channels());
        SwrContext* ctx = nullptr;
        if (swr_alloc_set_opts2(&ctx, &out, AV_SAMPLE_FMT_FLT, self->audioSink->sampleRate(), &in, AV_SAMPLE_FMT_S16,
                                static_cast<int>(rate), 0, nullptr) >= 0 &&
            swr_init(ctx) >= 0) {
            self->swr.reset(ctx);
        } else {
            swr_free(&ctx);
            log::warn("remoteplay: audio conversion unavailable ({} ch @ {} Hz)", channels, rate);
        }
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
    }

    static void onAudioFrame(int16_t* buf, size_t frames, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (!self->swr || !self->audioSink) return;
        const int outChannels = self->audioSink->channels();
        const int maxOut = swr_get_out_samples(self->swr.get(), static_cast<int>(frames));
        if (maxOut <= 0) return;
        std::vector<float> out(static_cast<std::size_t>(maxOut) * static_cast<std::size_t>(outChannels));
        auto* dst = reinterpret_cast<uint8_t*>(out.data());
        const auto* src = reinterpret_cast<const uint8_t*>(buf);
        const int n = swr_convert(self->swr.get(), &dst, maxOut, &src, static_cast<int>(frames));
        if (n <= 0) return;
        out.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(outChannels));
        const float vol = self->volume;
        if (vol != 1.0f) {
            for (float& s : out) s *= vol;
        }
        // Keep audio tight: if the device fell behind, drop the oldest chunk.
        if (self->pcm.size() >= 10) (void)self->pcm.tryPop();
        self->pcm.push(std::move(out));
    }

    // ---- threads ----
    void videoLoop() {
        std::optional<media::VideoDecoder> decoder;
        auto frame = media::makeFrame();
        auto windowStart = Clock::now();
        uint64_t windowFrames = 0, windowBytesStart = bytesReceived.load();
        double windowDecodeUs = 0;
        while (true) {
            auto sample = samples.popFor(std::chrono::milliseconds(100));
            if (!sample) {
                if (stopping()) return;
                continue;
            }
            if (resetDecoder.exchange(false) || !decoder) {
                std::string err;
                const AVCodecID id = chiaki_codec_is_h265(codec) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
                decoder = media::VideoDecoder::openStream(id, device, 3, true, &err);
                if (!decoder) {
                    log::error("remoteplay: {}", err);
                    setState(StreamState::Error, err);
                    requestStop();
                    return;
                }
                std::lock_guard lock(infoMutex);
                info.hardwareDecode = decoder->hardware();
            }
            if (keyframeWanted.exchange(false)) {
                log::info("remoteplay: decoder fell behind; backlog dropped, keyframe requested");
                requestKeyframeLocked();
            }
            const auto t0 = Clock::now();
            int rc = decoder->send(sample->packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                requestKeyframeLocked();
                continue;
            }
            while ((rc = decoder->receive(frame.get())) == 0) {
                presenter.present(*frame);
                av_frame_unref(frame.get());
                ++framesPresented;
                ++windowFrames;
                const auto now = Clock::now();
                const double decodeUs = std::chrono::duration<double, std::micro>(now - t0).count();
                decodeStage.record(decodeUs);
                latencyStage.record(std::chrono::duration<double, std::micro>(now - sample->arrived).count());
                windowDecodeUs += decodeUs;
            }
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed >= 1.0) {
                const uint64_t bytes = bytesReceived.load();
                std::lock_guard lock(infoMutex);
                info.fps = static_cast<double>(windowFrames) / elapsed;
                info.bitrateMbps = static_cast<double>(bytes - windowBytesStart) * 8.0 / elapsed / 1e6;
                info.decodeMs = windowFrames ? windowDecodeUs / static_cast<double>(windowFrames) / 1000.0 : 0.0;
                windowStart = now;
                windowFrames = 0;
                windowDecodeUs = 0;
                windowBytesStart = bytes;
            }
        }
    }

    void audioLoop() {
        while (auto chunk = pcm.pop()) {
            if (!audioSink) continue;
            const auto channels = static_cast<std::size_t>(audioSink->channels());
            if (!audioSink->write(chunk->data(), chunk->size() / channels, 0.0)) return;
        }
    }

    void inputLoop() {
        ChiakiControllerState last;
        chiaki_controller_state_set_idle(&last);
        auto lastSend = Clock::now();
        bool hadDevice = false;
        while (!stopping()) {
            GamepadState pad;
            const bool hasDevice = input && input->poll(pad);
            if (hasDevice != hadDevice) {
                hadDevice = hasDevice;
                std::lock_guard lock(infoMutex);
                info.gamepadConnected = hasDevice;
                info.gamepadName = hasDevice ? input->name() : std::string();
            }
            GamepadState over;
            {
                std::lock_guard lock(overlayMutex);
                over = overlay;
            }
            GamepadState merged = merge(pad, over);
            const auto now = Clock::now();
            if (now.time_since_epoch().count() < psUntil.load()) merged.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
            ChiakiControllerState state = toChiaki(merged);
            if (!chiaki_controller_state_equals(&state, &last) || now - lastSend > std::chrono::milliseconds(100)) {
                std::lock_guard lock(sessionMutex);
                if (sessionLive) chiaki_session_set_controller_state(&session, &state);
                last = state;
                lastSend = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4)); // 250 Hz
        }
    }

    bool stopping() {
        std::lock_guard lock(controlMutex);
        return stopRequested;
    }

    void requestStop() {
        std::lock_guard lock(controlMutex);
        stopRequested = true;
        controlCv.notify_all();
    }

    void requestKeyframeLocked() {
        std::lock_guard lock(sessionMutex);
        if (sessionLive) chiaki_session_request_idr(&session);
    }

    // One connection attempt. Returns the quit reason (STOPPED when the user stopped).
    ChiakiQuitReason runOnce(std::string& detail) {
        ChiakiConnectInfo ci{};
        ci.ps5 = console.kind == ConsoleKind::PS5;
        ci.host = console.host.c_str();
        std::memcpy(ci.regist_key, console.registKey.data(), sizeof(ci.regist_key));
        std::memcpy(ci.morning, console.rpKey.data(), sizeof(ci.morning));
        ci.video_profile.width = settings.width;
        ci.video_profile.height = settings.height;
        ci.video_profile.max_fps = settings.fps;
        ci.video_profile.bitrate = settings.bitrateKbps ? settings.bitrateKbps
                                   : settings.height >= 1080 ? 15000
                                   : settings.height >= 720  ? 10000
                                   : settings.height >= 540  ? 6000
                                                             : 2000;
        ci.video_profile.codec = static_cast<ChiakiCodec>(std::min<uint32_t>(settings.codec, 2));
        if (!ci.ps5 && ci.video_profile.codec != CHIAKI_CODEC_H264) ci.video_profile.codec = CHIAKI_CODEC_H264;
        ci.video_profile_auto_downgrade = true;
        ci.enable_keyboard = false;
        ci.enable_dualsense = settings.enableDualSense;
        ci.audio_video_disabled = CHIAKI_NONE_DISABLED;
        ci.auto_regist = false;
        ci.holepunch_session = nullptr;
        ci.rudp_sock = nullptr;
        std::memcpy(ci.psn_account_id, console.psnAccountId.data(), sizeof(ci.psn_account_id));
        ci.packet_loss_max = 0.05;
        ci.enable_idr_on_fec_failure = true;

        {
            std::lock_guard lock(controlMutex);
            quitReceived = false;
            quitReason = CHIAKI_QUIT_REASON_NONE;
            quitDetail.clear();
        }
        const auto err = chiaki_session_init(&session, &ci, chiakiLog());
        if (err != CHIAKI_ERR_SUCCESS) {
            detail = std::string("chiaki_session_init: ") + chiaki_error_string(err);
            return CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN;
        }
        chiaki_opus_decoder_init(&opus, chiakiLog());
        chiaki_opus_decoder_set_cb(&opus, &Impl::onAudioSettings, &Impl::onAudioFrame, this);
        ChiakiAudioSink sink{};
        chiaki_opus_decoder_get_sink(&opus, &sink);
        chiaki_session_set_audio_sink(&session, &sink);
        chiaki_session_set_video_sample_cb(&session, &Impl::onVideo, this);
        chiaki_session_set_event_cb(&session, &Impl::onEvent, this);

        resetDecoder = true;
        samples.clear();
        if (chiaki_session_start(&session) != CHIAKI_ERR_SUCCESS) {
            chiaki_opus_decoder_fini(&opus);
            chiaki_session_fini(&session);
            detail = "no se pudo iniciar la sesión";
            return CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN;
        }
        {
            std::lock_guard lock(sessionMutex);
            sessionLive = true;
        }

        ChiakiQuitReason reason;
        {
            std::unique_lock lock(controlMutex);
            controlCv.wait(lock, [&] { return stopRequested || quitReceived; });
            reason = quitReceived ? quitReason : CHIAKI_QUIT_REASON_STOPPED;
            detail = quitDetail;
            if (stopRequested) reason = CHIAKI_QUIT_REASON_STOPPED;
        }
        {
            std::lock_guard lock(sessionMutex);
            sessionLive = false;
        }
        chiaki_session_stop(&session);
        chiaki_session_join(&session);
        chiaki_session_fini(&session);
        chiaki_opus_decoder_fini(&opus);
        return reason;
    }

    void controlLoop() {
        std::string initError;
        if (!ensureChiakiInitialized(&initError)) {
            setState(StreamState::Error, initError);
            return;
        }
        if (audioFactory) audioSink = audioFactory();
        video = std::thread([this] { videoLoop(); });
        audioThread = std::thread([this] { audioLoop(); });
        inputThread = std::thread([this] { inputLoop(); });

        int attempt = 0;
        bool woke = false;
        bool everStreamed = false;
        while (!stopping()) {
            setState(attempt == 0 ? StreamState::Connecting : StreamState::Reconnecting);
            std::string detail;
            const auto reason = runOnce(detail);
            if (stopping() || reason == CHIAKI_QUIT_REASON_STOPPED) {
                if (!detail.empty()) setState(StreamState::Error, detail);
                break;
            }
            bool streamed;
            {
                std::lock_guard lock(infoMutex);
                streamed = info.state == StreamState::Streaming;
            }
            if (streamed) {
                everStreamed = true;
                attempt = 0; // a session that worked gets a fresh retry budget
            }
            const std::string text = quitText(reason, detail.c_str());
            log::warn("remoteplay: session ended: {}", text);

            // Console asleep or unreachable: wake it once and give it time to boot.
            const bool connectFailure = reason == CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED ||
                                        reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN ||
                                        reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED;
            if (connectFailure && settings.wakeIfAsleep && !woke) {
                woke = true;
                setState(StreamState::Waking);
                std::string wakeErr;
                if (wakeConsole(console, &wakeErr)) {
                    if (waitOrStop(std::chrono::seconds(14))) break;
                    continue;
                }
                log::warn("remoteplay: {}", wakeErr);
            }
            if (!settings.autoReconnect || !shouldRetry(reason) || ++attempt > 6) {
                setState(everStreamed && reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN
                             ? StreamState::Ended
                             : StreamState::Error,
                         text);
                break;
            }
            ++reconnects;
            {
                std::lock_guard lock(infoMutex);
                info.state = StreamState::Reconnecting;
                info.error = text;
                info.reconnects = reconnects;
            }
            const int backoff = reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE ? 5 : std::min(1 << (attempt - 1), 8);
            if (waitOrStop(std::chrono::seconds(backoff))) break;
        }
        {
            std::lock_guard lock(infoMutex);
            if (info.state != StreamState::Error && info.state != StreamState::Ended) info.state = StreamState::Ended;
        }
        requestStop();
        samples.abort();
        pcm.abort();
        if (audioSink) audioSink->stop();
        if (video.joinable()) video.join();
        if (audioThread.joinable()) audioThread.join();
        if (inputThread.joinable()) inputThread.join();
        if (input) input->rumble(0, 0);
    }

    // true if stop was requested while waiting.
    template <class D>
    bool waitOrStop(D duration) {
        std::unique_lock lock(controlMutex);
        return controlCv.wait_for(lock, duration, [&] { return stopRequested; });
    }
};

StreamSession::StreamSession(ID3D12Device* device, player::IVideoPresenter& presenter, AudioSinkFactory audio,
                             std::unique_ptr<IInputDevice> input, telemetry::Telemetry& telemetry)
    : impl_(std::make_unique<Impl>(device, presenter, std::move(audio), std::move(input), telemetry)) {}

StreamSession::~StreamSession() { stop(); }

void StreamSession::start(const ConsoleProfile& console, const StreamSettings& settings) {
    stop();
    auto& i = *impl_;
    i.console = console;
    i.settings = settings;
    {
        std::lock_guard lock(i.controlMutex);
        i.stopRequested = false;
    }
    i.samples.reset();
    i.pcm.reset();
    i.bytesReceived = 0;
    i.framesPresented = 0;
    i.framesLost = 0;
    i.framesRecovered = 0;
    i.reconnects = 0;
    {
        std::lock_guard lock(i.infoMutex);
        i.info = StreamSnapshot{};
        i.info.state = StreamState::Connecting;
    }
    i.control = std::thread([&i] { i.controlLoop(); });
}

void StreamSession::stop() {
    auto& i = *impl_;
    i.requestStop();
    if (i.control.joinable()) i.control.join();
}

void StreamSession::setOverlayInput(const GamepadState& state) {
    std::lock_guard lock(impl_->overlayMutex);
    impl_->overlay = state;
}

void StreamSession::setVolume(float volume) { impl_->volume = std::clamp(volume, 0.0f, 1.0f); }

void StreamSession::requestKeyframe() { impl_->requestKeyframeLocked(); }

void StreamSession::sendPsButton() {
    impl_->psUntil = (Clock::now() + std::chrono::milliseconds(120)).time_since_epoch().count();
}

void StreamSession::putConsoleToSleep() {
    std::lock_guard lock(impl_->sessionMutex);
    if (impl_->sessionLive) chiaki_session_goto_bed(&impl_->session);
}

StreamSnapshot StreamSession::snapshot() const {
    auto& i = *impl_;
    std::lock_guard lock(i.infoMutex);
    StreamSnapshot s = i.info;
    s.framesPresented = i.framesPresented;
    s.framesLost = i.framesLost;
    s.framesRecovered = i.framesRecovered;
    s.reconnects = i.reconnects;
    return s;
}

} // namespace bv::remote
