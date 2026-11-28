#include "SubtitleDecoder.h"

#include <algorithm>

namespace bv::media {

bool SubtitleDecoder::isText(AVCodecID id) {
    const auto* d = avcodec_descriptor_get(id);
    return d && d->type == AVMEDIA_TYPE_SUBTITLE && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

std::optional<SubtitleDecoder> SubtitleDecoder::open(const AVCodecParameters* params, AVRational timeBase) {
    if (!params || !isText(params->codec_id)) return std::nullopt;
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return std::nullopt;
    SubtitleDecoder d;
    d.ctx_.reset(avcodec_alloc_context3(codec));
    if (!d.ctx_ || avcodec_parameters_to_context(d.ctx_.get(), params) < 0) return std::nullopt;
    d.ctx_->pkt_timebase = timeBase;
    if (avcodec_open2(d.ctx_.get(), codec, nullptr) < 0) return std::nullopt;
    d.timeBase_ = timeBase;
    return d;
}

void SubtitleDecoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_.get());
}

void SubtitleDecoder::decode(const AVPacket* packet, std::vector<SubtitleCue>& out) {
    if (!ctx_ || !packet) return;
    AVSubtitle sub{};
    int got = 0;
    if (avcodec_decode_subtitle2(ctx_.get(), &sub, &got, const_cast<AVPacket*>(packet)) < 0 || !got) return;

    const double base = sub.pts != AV_NOPTS_VALUE ? static_cast<double>(sub.pts) / AV_TIME_BASE
                        : packet->pts != AV_NOPTS_VALUE ? static_cast<double>(packet->pts) * av_q2d(timeBase_)
                                                        : 0.0;
    SubtitleCue cue;
    cue.start = base + sub.start_display_time / 1000.0;
    if (sub.end_display_time > sub.start_display_time && sub.end_display_time != UINT32_MAX) {
        cue.end = base + sub.end_display_time / 1000.0;
    } else if (packet->duration > 0) {
        cue.end = cue.start + static_cast<double>(packet->duration) * av_q2d(timeBase_);
    } else {
        cue.end = cue.start + 6.0;
        cue.openEnded = true;
    }
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        std::string line;
        if (r->type == SUBTITLE_ASS && r->ass) line = assToPlain(r->ass);
        else if (r->type == SUBTITLE_TEXT && r->text) line = r->text;
        if (line.empty()) continue;
        if (!cue.text.empty()) cue.text += '\n';
        cue.text += line;
    }
    avsubtitle_free(&sub);
    if (!cue.text.empty()) out.push_back(std::move(cue));
}

std::string assToPlain(std::string_view event) {
    // Skip the eight event fields before the text, when present.
    std::string_view text = event;
    {
        std::size_t commas = 0, at = 0;
        for (; at < event.size() && commas < 8; ++at)
            if (event[at] == ',') ++commas;
        if (commas == 8) text = event.substr(at);
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') { // override block
            const auto close = text.find('}', i);
            if (close == std::string_view::npos) break;
            i = close;
            continue;
        }
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == 'N' || n == 'n') { out += '\n'; ++i; continue; }
            if (n == 'h') { out += ' '; ++i; continue; }
        }
        if (c == '\r') continue;
        out += c;
    }
    // Trim spaces and empty lines at both ends.
    const auto first = out.find_first_not_of(" \n\t");
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(" \n\t");
    return out.substr(first, last - first + 1);
}

} // namespace bv::media
