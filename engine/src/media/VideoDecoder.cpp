#include "VideoDecoder.h"

#include "core/Log.h"

#include <thread>

#if defined(_WIN32) && __has_include(<libavutil/hwcontext_d3d12va.h>)
#  define BV_HAS_D3D12VA 1
#  include <d3d12.h>
extern "C" {
#  include <libavutil/hwcontext_d3d12va.h>
}
#endif

namespace bv::media {
namespace {

#if defined(BV_HAS_D3D12VA)
AVPixelFormat pickD3D12(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D12) return *p;
    }
    log::warn("decoder: D3D12 surface format not offered, falling back to software");
    return formats[0];
}

AVBufferRef* makeD3D12Device(ID3D12Device* device) {
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
    if (!ref) return nullptr;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3d = static_cast<AVD3D12VADeviceContext*>(hw->hwctx);
    device->AddRef(); // FFmpeg releases it when the context is freed
    d3d->device = device;
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return nullptr;
    }
    return ref;
}
#endif

} // namespace

std::optional<VideoDecoder> VideoDecoder::open(const StreamInfo& stream, ID3D12Device* device, int extraFrames,
                                               std::string* error) {
    auto fail = [&](const std::string& msg) -> std::optional<VideoDecoder> {
        if (error) *error = msg;
        return std::nullopt;
    };
    const AVCodec* codec = avcodec_find_decoder(stream.params->codec_id);
    if (!codec) return fail(std::string("códec no soportado: ") + avcodec_get_name(stream.params->codec_id));

    VideoDecoder d;
    d.ctx_.reset(avcodec_alloc_context3(codec));
    if (!d.ctx_ || avcodec_parameters_to_context(d.ctx_.get(), stream.params) < 0) return fail("contexto de códec");
    d.ctx_->pkt_timebase = stream.timeBase;

#if defined(BV_HAS_D3D12VA)
    if (device) {
        if (AVBufferRef* hw = makeD3D12Device(device)) {
            d.hwDevice_.reset(hw);
            d.ctx_->hw_device_ctx = av_buffer_ref(hw);
            d.ctx_->get_format = pickD3D12;
            d.ctx_->extra_hw_frames = extraFrames;
            d.hardware_ = true;
        } else {
            log::warn("decoder: D3D12VA device init failed, using software decode");
        }
    }
#else
    (void)device;
    (void)extraFrames;
#endif

    if (!d.hardware_) {
        // Software path: use every core, frame threading keeps latency acceptable for playback.
        d.ctx_->thread_count = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
        d.ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    const int err = avcodec_open2(d.ctx_.get(), codec, nullptr);
    if (err < 0) return fail("no se pudo abrir el decodificador: " + errorString(err));
    log::info("decoder: {} {}x{} ({})", codec->name, d.ctx_->width, d.ctx_->height,
              d.hardware_ ? "D3D12 Video" : "software");
    return d;
}

} // namespace bv::media
