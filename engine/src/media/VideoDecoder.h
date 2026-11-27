#pragma once

#include "Demuxer.h"
#include "FFmpeg.h"

#include <optional>
#include <string>

struct ID3D12Device;

namespace bv::media {

// Video decoder. With a D3D12 device it decodes on the GPU (D3D12 Video via FFmpeg's
// d3d12va hwaccel) and frames stay in VRAM as AV_PIX_FMT_D3D12; otherwise software.
class VideoDecoder {
public:
    // extraFrames: surfaces the consumer may hold at once (render queue depth).
    static std::optional<VideoDecoder> open(const StreamInfo& stream, ID3D12Device* device, int extraFrames,
                                            std::string* error);

    // Raw elementary stream (Annex B), e.g. a Remote Play feed. lowLatency: output every
    // frame as soon as it is decoded (no reordering delay, no frame threading).
    static std::optional<VideoDecoder> openStream(AVCodecID codecId, ID3D12Device* device, int extraFrames,
                                                  bool lowLatency, std::string* error);

    VideoDecoder(VideoDecoder&&) noexcept = default;
    VideoDecoder& operator=(VideoDecoder&&) noexcept = default;

    // Thin wrappers over avcodec_send_packet / avcodec_receive_frame.
    int send(const AVPacket* packet) { return avcodec_send_packet(ctx_.get(), packet); }
    int receive(AVFrame* frame) { return avcodec_receive_frame(ctx_.get(), frame); }
    void flush() { avcodec_flush_buffers(ctx_.get()); }

    [[nodiscard]] bool hardware() const { return hardware_; }
    [[nodiscard]] const char* codecName() const { return ctx_->codec ? ctx_->codec->name : "?"; }
    [[nodiscard]] AVCodecContext* context() const { return ctx_.get(); }

private:
    VideoDecoder() = default;
    static std::optional<VideoDecoder> create(const AVCodec* codec, const AVCodecParameters* params, AVRational timeBase,
                                              ID3D12Device* device, int extraFrames, bool lowLatency,
                                              std::string* error);
    CodecCtxPtr ctx_;
    BufferRefPtr hwDevice_;
    bool hardware_ = false;
};

} // namespace bv::media
