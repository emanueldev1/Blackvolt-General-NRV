#include "AudioDecoder.h"

#include <cmath>
#include <limits>

namespace bv::media {

std::optional<AudioDecoder> AudioDecoder::open(const StreamInfo& stream, int outRate, int outChannels,
                                               std::string* error) {
    auto fail = [&](const std::string& msg) -> std::optional<AudioDecoder> {
        if (error) *error = msg;
        return std::nullopt;
    };
    const AVCodec* codec = avcodec_find_decoder(stream.params->codec_id);
    if (!codec) return fail("códec de audio no soportado");

    AudioDecoder d;
    d.ctx_.reset(avcodec_alloc_context3(codec));
    if (!d.ctx_ || avcodec_parameters_to_context(d.ctx_.get(), stream.params) < 0) return fail("contexto de audio");
    d.ctx_->pkt_timebase = stream.timeBase;
    if (int err = avcodec_open2(d.ctx_.get(), codec, nullptr); err < 0) return fail("audio: " + errorString(err));

    d.timeBase_ = stream.timeBase;
    d.outRate_ = outRate;
    d.outChannels_ = outChannels;
    d.frame_ = makeFrame();

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels);
    AVChannelLayout inLayout = d.ctx_->ch_layout;
    if (inLayout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&inLayout, inLayout.nb_channels);

    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout, d.ctx_->sample_fmt,
                            d.ctx_->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr) < 0) {
        swr_free(&swr);
        return fail("no se pudo configurar el remuestreo de audio");
    }
    d.swr_.reset(swr);
    return d;
}

int AudioDecoder::receive(std::vector<float>& out, double& pts) {
    const int err = avcodec_receive_frame(ctx_.get(), frame_.get());
    if (err < 0) return err;

    pts = frame_->best_effort_timestamp == AV_NOPTS_VALUE
              ? std::numeric_limits<double>::quiet_NaN()
              : static_cast<double>(frame_->best_effort_timestamp) * av_q2d(timeBase_);

    const int maxOut = swr_get_out_samples(swr_.get(), frame_->nb_samples);
    const auto start = out.size();
    out.resize(start + static_cast<std::size_t>(maxOut) * outChannels_);
    auto* dst = reinterpret_cast<uint8_t*>(out.data() + start);
    const int got = swr_convert(swr_.get(), &dst, maxOut, const_cast<const uint8_t**>(frame_->extended_data),
                                frame_->nb_samples);
    out.resize(start + static_cast<std::size_t>(std::max(got, 0)) * outChannels_);
    av_frame_unref(frame_.get());
    return 0;
}

void AudioDecoder::flush() {
    avcodec_flush_buffers(ctx_.get());
    swr_init(swr_.get()); // drop buffered samples
}

} // namespace bv::media
