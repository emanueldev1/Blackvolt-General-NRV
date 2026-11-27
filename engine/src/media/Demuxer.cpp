#include "Demuxer.h"

#include "CaptureUrl.h"

#if __has_include(<libavdevice/avdevice.h>)
extern "C" {
#include <libavdevice/avdevice.h>
}
#define BV_HAS_AVDEVICE 1
#endif

#include <mutex>

namespace bv::media {

std::optional<Demuxer> Demuxer::open(const std::string& urlUtf8, std::string* error) {
    auto fail = [&](const std::string& msg) -> std::optional<Demuxer> {
        if (error) *error = msg;
        return std::nullopt;
    };

    AVFormatContext* raw = nullptr;
    AVDictionary* opts = nullptr;
    const AVInputFormat* format = nullptr;
    std::string target = urlUtf8;
    const bool live = isCaptureUrl(urlUtf8);
    if (live) {
#if defined(BV_HAS_AVDEVICE)
        static std::once_flag once;
        std::call_once(once, [] { avdevice_register_all(); });
        std::string parseError;
        const auto capture = parseCaptureUrl(urlUtf8, &parseError);
        if (!capture) return fail(parseError);
        format = av_find_input_format(capture->inputFormat.c_str());
        if (!format) return fail("esta compilación no incluye la entrada " + capture->inputFormat);
        target = capture->target;
        for (const auto& [k, v] : capture->options) av_dict_set(&opts, k.c_str(), v.c_str(), 0);
        // Live: start as soon as the first frames arrive.
        av_dict_set(&opts, "probesize", "500000", 0);
        av_dict_set(&opts, "analyzeduration", "0", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
#else
        return fail("esta compilación no incluye captura de dispositivos");
#endif
    } else {
        av_dict_set(&opts, "probesize", "10000000", 0);
        av_dict_set(&opts, "analyzeduration", "5000000", 0);
    }
    int err = avformat_open_input(&raw, target.c_str(), format, &opts);
    av_dict_free(&opts);
    if (err < 0) return fail((live ? "no se pudo abrir el dispositivo: " : "no se pudo abrir: ") + errorString(err));

    Demuxer d;
    d.fmt_.reset(raw);
    d.live_ = live;
    if ((err = avformat_find_stream_info(raw, nullptr)) < 0) return fail("sin información de streams: " + errorString(err));

    const int v = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) return fail("el archivo no tiene video");
    d.video_ = {v, raw->streams[v]->time_base, raw->streams[v]->codecpar};

    const int a = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
    if (a >= 0) d.audio_ = {a, raw->streams[a]->time_base, raw->streams[a]->codecpar};

    // Drop packets of streams we never decode as early as possible.
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        if (static_cast<int>(i) != v && static_cast<int>(i) != a) raw->streams[i]->discard = AVDISCARD_ALL;
    }
    return d;
}

double Demuxer::durationSeconds() const {
    if (!fmt_ || fmt_->duration <= 0) return 0.0;
    return static_cast<double>(fmt_->duration) / AV_TIME_BASE;
}

double Demuxer::frameRate() const {
    if (!fmt_ || video_.index < 0) return 0.0;
    const auto* s = fmt_->streams[video_.index];
    AVRational r = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
    return r.den ? av_q2d(r) : 0.0;
}

int Demuxer::read(AVPacket* packet) {
    for (;;) {
        const int err = av_read_frame(fmt_.get(), packet);
        if (err < 0) return err;
        if (packet->stream_index == video_.index || packet->stream_index == audio_.index) return 0;
        av_packet_unref(packet);
    }
}

bool Demuxer::seek(double seconds) {
    if (live_) return false;
    const auto ts = static_cast<int64_t>(seconds * AV_TIME_BASE);
    return avformat_seek_file(fmt_.get(), -1, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD) >= 0;
}

} // namespace bv::media
