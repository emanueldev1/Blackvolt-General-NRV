#include "Demuxer.h"

namespace bv::media {

std::optional<Demuxer> Demuxer::open(const std::string& urlUtf8, std::string* error) {
    auto fail = [&](const std::string& msg) -> std::optional<Demuxer> {
        if (error) *error = msg;
        return std::nullopt;
    };

    AVFormatContext* raw = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "10000000", 0);
    av_dict_set(&opts, "analyzeduration", "5000000", 0);
    int err = avformat_open_input(&raw, urlUtf8.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) return fail("no se pudo abrir: " + errorString(err));

    Demuxer d;
    d.fmt_.reset(raw);
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
    const auto ts = static_cast<int64_t>(seconds * AV_TIME_BASE);
    return avformat_seek_file(fmt_.get(), -1, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD) >= 0;
}

} // namespace bv::media
