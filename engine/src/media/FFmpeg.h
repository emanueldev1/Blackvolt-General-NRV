#pragma once

// Single include point for FFmpeg (C headers) plus small RAII helpers.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#include <memory>
#include <string>

namespace bv::media {

struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct CodecCtxDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FormatCtxDeleter { void operator()(AVFormatContext* f) const { avformat_close_input(&f); } };
struct SwrDeleter { void operator()(SwrContext* s) const { swr_free(&s); } };
struct BufferRefDeleter { void operator()(AVBufferRef* b) const { av_buffer_unref(&b); } };

using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }
inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }

inline std::string errorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

} // namespace bv::media
