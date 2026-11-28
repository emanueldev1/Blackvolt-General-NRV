#include "Exporter.h"

#include "core/Log.h"
#include "media/FFmpeg.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <thread>

namespace bv::exporter {

using namespace bv::media;

namespace {

struct SwsDeleter { void operator()(SwsContext* s) const { sws_freeContext(s); } };
struct OutputDeleter {
    void operator()(AVFormatContext* f) const {
        if (!f) return;
        if (f->pb && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb);
        avformat_free_context(f);
    }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using OutputPtr = std::unique_ptr<AVFormatContext, OutputDeleter>;

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

bool isHardware(std::string_view name) {
    return contains(name, "nvenc") || contains(name, "_amf") || contains(name, "_qsv") || contains(name, "_mf");
}

std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// Encoder-specific settings for a 0..100 quality value.
AVPixelFormat configure(AVCodecContext* enc, const std::string& name, uint32_t quality, bool tenBit,
                        AVDictionary** opts) {
    const double q = std::clamp<double>(quality, 0, 100);
    const int cq = static_cast<int>(std::lround(45.0 - q * 0.3)); // 70 → 24, 100 → 15
    const auto set = [&](const char* k, const std::string& v) { av_dict_set(opts, k, v.c_str(), 0); };

    if (contains(name, "nvenc")) {
        set("preset", "p5");
        set("tune", "hq");
        set("rc", "vbr");
        set("cq", std::to_string(cq));
        set("spatial-aq", "1");
        enc->bit_rate = 0;
        return tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    }
    // Hardware encoders without a constant-quality mode get a bitrate scaled to the picture.
    if (isHardware(name)) {
        const double fps = enc->framerate.num ? av_q2d(enc->framerate) : 30.0;
        const double bpp = 0.04 + q / 100.0 * 0.12;
        enc->bit_rate = static_cast<int64_t>(enc->width * static_cast<double>(enc->height) * fps * bpp);
        if (contains(name, "_mf")) set("rate_control", "quality");
        return AV_PIX_FMT_NV12;
    }
    if (name == "libx264" || name == "libx265") {
        set("preset", "medium");
        set("crf", std::to_string(cq - 4));
        return AV_PIX_FMT_YUV420P;
    }
    if (name == "libsvtav1" || name == "libaom-av1") {
        set("crf", std::to_string(std::lround(55.0 - q * 0.35)));
        if (name == "libsvtav1") set("preset", "8");
        enc->bit_rate = 0;
        return AV_PIX_FMT_YUV420P;
    }
    // mpeg4 and other basic encoders: fixed quantiser.
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = FF_QP2LAMBDA * static_cast<int>(std::lround(31.0 - q * 0.29));
    return AV_PIX_FMT_YUV420P;
}

} // namespace

std::vector<std::string> defaultEncoders(Codec codec) {
    switch (codec) {
    case Codec::H264: return {"h264_nvenc", "h264_amf", "h264_mf", "libx264", "libopenh264", "mpeg4"};
    case Codec::Hevc: return {"hevc_nvenc", "hevc_amf", "hevc_mf", "libx265", "h264_nvenc", "libx264", "mpeg4"};
    case Codec::Av1: return {"av1_nvenc", "av1_amf", "av1_mf", "libsvtav1", "libaom-av1", "hevc_nvenc", "libx264", "mpeg4"};
    }
    return {"mpeg4"};
}

void fitSize(uint32_t srcW, uint32_t srcH, uint32_t reqW, uint32_t reqH, uint32_t& outW, uint32_t& outH) {
    double w = srcW, h = srcH;
    if (srcW && srcH) {
        if (reqW && reqH) {
            const double s = std::min(reqW / static_cast<double>(srcW), reqH / static_cast<double>(srcH));
            w = srcW * s;
            h = srcH * s;
        } else if (reqW) {
            w = reqW;
            h = srcH * (reqW / static_cast<double>(srcW));
        } else if (reqH) {
            h = reqH;
            w = srcW * (reqH / static_cast<double>(srcH));
        }
    }
    outW = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(w / 2.0)) * 2);
    outH = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(h / 2.0)) * 2);
}

Job::~Job() {
    cancel_ = true;
    wait();
}

void Job::wait() {
    if (thread_.joinable()) thread_.join();
}

void Job::start(Options options) {
    cancel_ = true;
    wait();
    cancel_ = false;
    options_ = std::move(options);
    {
        std::lock_guard lock(mutex_);
        progress_ = {};
        progress_.state = State::Running;
    }
    thread_ = std::thread([this] { run(); });
}

Progress Job::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

void Job::update(const std::function<void(Progress&)>& fn) {
    std::lock_guard lock(mutex_);
    fn(progress_);
}

void Job::run() {
    const auto started = std::chrono::steady_clock::now();
    const auto fail = [&](const std::string& msg) {
        update([&](Progress& p) {
            p.state = State::Failed;
            p.message = msg;
        });
    };

    if (options_.input.empty() || options_.output.empty()) return fail("faltan la entrada o la salida");
    {
        std::error_code ec;
        if (std::filesystem::equivalent(pathFromUtf8(options_.input), pathFromUtf8(options_.output), ec))
            return fail("la salida no puede ser el mismo archivo que la entrada");
    }

    // ---- input ------------------------------------------------------------------------------
    AVFormatContext* rawIn = nullptr;
    int err = avformat_open_input(&rawIn, options_.input.c_str(), nullptr, nullptr);
    if (err < 0) return fail("no se pudo abrir: " + errorString(err));
    FormatCtxPtr in(rawIn);
    if ((err = avformat_find_stream_info(in.get(), nullptr)) < 0) return fail("sin información de streams: " + errorString(err));

    const int vIndex = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIndex < 0) return fail("el archivo no tiene video");
    const int aIndex = options_.audio ? av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, vIndex, nullptr, 0) : -1;
    AVStream* inVideo = in->streams[vIndex];

    const AVCodec* decoder = avcodec_find_decoder(inVideo->codecpar->codec_id);
    if (!decoder) return fail("no hay decodificador para este video");
    CodecCtxPtr dec(avcodec_alloc_context3(decoder));
    avcodec_parameters_to_context(dec.get(), inVideo->codecpar);
    const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 16u));
    dec->thread_count = threads;
    dec->pkt_timebase = inVideo->time_base;
    if ((err = avcodec_open2(dec.get(), decoder, nullptr)) < 0) return fail("no se pudo abrir el decodificador: " + errorString(err));

    uint32_t outW = 0, outH = 0;
    fitSize(static_cast<uint32_t>(dec->width), static_cast<uint32_t>(dec->height), options_.width, options_.height, outW, outH);

    AVRational frameRate = av_guess_frame_rate(in.get(), inVideo, nullptr);
    if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = {30, 1};
    // Optional GPU filter (DLSS NR) at the source size, before scaling to the output size.
    std::unique_ptr<FrameFilter> filter;
    if (options_.filter) {
        std::string why;
        filter = options_.filter(static_cast<uint32_t>(dec->width), static_cast<uint32_t>(dec->height),
                                 1000.0 / av_q2d(frameRate), &why);
        if (!filter) return fail("DLSS NR: " + why);
        update([&](Progress& p) { p.enhance = filter->describe(); });
    }
    const auto* srcDesc = av_pix_fmt_desc_get(dec->pix_fmt);
    const bool tenBit = srcDesc && srcDesc->comp[0].depth > 8 && options_.codec != Codec::H264;

    // ---- output -----------------------------------------------------------------------------
    AVFormatContext* rawOut = nullptr;
    if ((err = avformat_alloc_output_context2(&rawOut, nullptr, nullptr, options_.output.c_str())) < 0 || !rawOut)
        return fail("formato de salida no reconocido (usa .mp4, .mkv o .mov)");
    OutputPtr out(rawOut);

    // First encoder in the list that opens wins: hardware first, software as the safety net.
    const auto names = options_.encoders.empty() ? defaultEncoders(options_.codec) : options_.encoders;
    CodecCtxPtr enc;
    std::string encoderName;
    std::string lastError = "no hay ningún codificador disponible";
    for (const auto& name : names) {
        if (cancel_) break;
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) continue;
        if (avformat_query_codec(out->oformat, codec->id, FF_COMPLIANCE_NORMAL) != 1) continue;
        CodecCtxPtr ctx(avcodec_alloc_context3(codec));
        ctx->width = static_cast<int>(outW);
        ctx->height = static_cast<int>(outH);
        ctx->framerate = frameRate;
        ctx->time_base = name == "mpeg4" ? av_inv_q(frameRate) : inVideo->time_base;
        ctx->sample_aspect_ratio = {1, 1};
        ctx->color_primaries = dec->color_primaries;
        ctx->color_trc = dec->color_trc;
        ctx->colorspace = dec->colorspace;
        ctx->color_range = dec->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_MPEG : dec->color_range;
        ctx->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(frameRate) * 2)));
        ctx->thread_count = threads;
        if (out->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = nullptr;
        ctx->pix_fmt = configure(ctx.get(), name, options_.quality, tenBit, &opts);
        err = avcodec_open2(ctx.get(), codec, &opts);
        av_dict_free(&opts);
        if (err >= 0) {
            bv::log::info("export: {} {}x{} → {} {}x{}", avcodec_get_name(inVideo->codecpar->codec_id), dec->width,
                          dec->height, name, outW, outH);
            enc = std::move(ctx);
            encoderName = name;
            break;
        }
        lastError = name + ": " + errorString(err);
    }
    if (cancel_) {
        update([](Progress& p) { p.state = State::Cancelled; });
        return;
    }
    if (!enc) return fail(lastError);

    update([&](Progress& p) {
        p.encoder = encoderName;
        p.hardware = isHardware(encoderName);
        p.outWidth = outW;
        p.outHeight = outH;
    });

    AVStream* outVideo = avformat_new_stream(out.get(), nullptr);
    avcodec_parameters_from_context(outVideo->codecpar, enc.get());
    outVideo->time_base = enc->time_base;
    outVideo->avg_frame_rate = frameRate;

    AVStream* outAudio = nullptr;
    std::string note;
    if (aIndex >= 0) {
        const auto* apar = in->streams[aIndex]->codecpar;
        if (avformat_query_codec(out->oformat, apar->codec_id, FF_COMPLIANCE_NORMAL) == 1) {
            outAudio = avformat_new_stream(out.get(), nullptr);
            avcodec_parameters_copy(outAudio->codecpar, apar);
            outAudio->codecpar->codec_tag = 0;
            outAudio->time_base = in->streams[aIndex]->time_base;
        } else {
            note = std::string("audio omitido: ") + avcodec_get_name(apar->codec_id) +
                   " no cabe en este contenedor (prueba con .mkv)";
        }
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        if ((err = avio_open(&out->pb, options_.output.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("no se pudo crear el archivo: " + errorString(err));
    }
    {
        AVDictionary* mux = nullptr;
        if (contains(out->oformat->name, "mp4") || contains(out->oformat->name, "mov"))
            av_dict_set(&mux, "movflags", "+faststart", 0);
        err = avformat_write_header(out.get(), &mux);
        av_dict_free(&mux);
        if (err < 0) return fail("no se pudo escribir la cabecera: " + errorString(err));
    }

    // ---- pipeline ---------------------------------------------------------------------------
    const double duration = in->duration > 0 ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;
    const double startTime = inVideo->start_time != AV_NOPTS_VALUE ? inVideo->start_time * av_q2d(inVideo->time_base) : 0.0;
    auto packet = makePacket();
    auto decoded = makeFrame();
    auto scaled = makeFrame();
    scaled->format = enc->pix_fmt;
    scaled->width = enc->width;
    scaled->height = enc->height;
    if (av_frame_get_buffer(scaled.get(), 0) < 0) return fail("sin memoria para el fotograma");
    SwsContext* sws = nullptr;
    SwsPtr swsOwner;
    // Filter path: decoded -> RGBA (source size) -> filter -> RGBA -> encoder format (output size).
    auto rgbaIn = makeFrame();
    auto rgbaOut = makeFrame();
    SwsContext* toRgb = nullptr;
    SwsContext* fromRgb = nullptr;
    SwsPtr toRgbOwner, fromRgbOwner;
    if (filter) {
        for (auto* f : {rgbaIn.get(), rgbaOut.get()}) {
            f->format = AV_PIX_FMT_RGBA;
            f->width = dec->width;
            f->height = dec->height;
            if (av_frame_get_buffer(f, 0) < 0) return fail("sin memoria para el fotograma");
        }
    }
    int64_t lastPts = AV_NOPTS_VALUE;
    int64_t nextPts = 0;
    uint64_t frames = 0;
    bool writeFailed = false;

    const auto drainEncoder = [&](const AVFrame* frame) {
        if (avcodec_send_frame(enc.get(), frame) < 0) return;
        auto encoded = makePacket();
        while (avcodec_receive_packet(enc.get(), encoded.get()) >= 0) {
            encoded->stream_index = outVideo->index;
            av_packet_rescale_ts(encoded.get(), enc->time_base, outVideo->time_base);
            if (av_interleaved_write_frame(out.get(), encoded.get()) < 0) writeFailed = true;
        }
    };

    bool filterFailed = false;
    const auto encodeDecoded = [&]() {
        while (avcodec_receive_frame(dec.get(), decoded.get()) >= 0) {
            const int flags = (static_cast<uint32_t>(decoded->width) != outW || static_cast<uint32_t>(decoded->height) != outH)
                                  ? SWS_LANCZOS | SWS_ACCURATE_RND
                                  : SWS_BICUBIC | SWS_ACCURATE_RND;
            if (filter && decoded->width == rgbaIn->width && decoded->height == rgbaIn->height) {
                // Same matrix and range on the way in and out, so the colours round-trip exactly.
                int cs = decoded->colorspace;
                if (cs != SWS_CS_ITU709 && cs != SWS_CS_ITU601 && cs != SWS_CS_SMPTE170M && cs != SWS_CS_SMPTE240M &&
                    cs != SWS_CS_BT2020 && cs != SWS_CS_FCC)
                    cs = decoded->height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
                const int full = decoded->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
                toRgb = sws_getCachedContext(toRgb, decoded->width, decoded->height,
                                             static_cast<AVPixelFormat>(decoded->format), decoded->width, decoded->height,
                                             AV_PIX_FMT_RGBA, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT,
                                             nullptr, nullptr, nullptr);
                fromRgb = sws_getCachedContext(fromRgb, decoded->width, decoded->height, AV_PIX_FMT_RGBA, enc->width,
                                               enc->height, enc->pix_fmt, flags, nullptr, nullptr, nullptr);
                if (toRgbOwner.get() != toRgb) { toRgbOwner.release(); toRgbOwner.reset(toRgb); }
                if (fromRgbOwner.get() != fromRgb) { fromRgbOwner.release(); fromRgbOwner.reset(fromRgb); }
                if (!toRgb || !fromRgb || av_frame_make_writable(scaled.get()) < 0) {
                    av_frame_unref(decoded.get());
                    continue;
                }
                const int* coeffs = sws_getCoefficients(cs);
                sws_setColorspaceDetails(toRgb, coeffs, full, coeffs, 1, 0, 1 << 16, 1 << 16);
                sws_setColorspaceDetails(fromRgb, coeffs, 1, coeffs, full, 0, 1 << 16, 1 << 16);
                sws_scale(toRgb, decoded->data, decoded->linesize, 0, decoded->height, rgbaIn->data, rgbaIn->linesize);
                if (!filter->process(rgbaIn->data[0], rgbaIn->linesize[0], rgbaOut->data[0], rgbaOut->linesize[0])) {
                    filterFailed = true;
                    av_frame_unref(decoded.get());
                    return;
                }
                sws_scale(fromRgb, rgbaOut->data, rgbaOut->linesize, 0, decoded->height, scaled->data, scaled->linesize);
            } else {
                sws = sws_getCachedContext(sws, decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format),
                                           enc->width, enc->height, enc->pix_fmt, flags, nullptr, nullptr, nullptr);
                if (swsOwner.get() != sws) {
                    swsOwner.release();
                    swsOwner.reset(sws);
                }
                if (!sws || av_frame_make_writable(scaled.get()) < 0) {
                    av_frame_unref(decoded.get());
                    continue;
                }
                sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height, scaled->data, scaled->linesize);
            }

            int64_t pts = decoded->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) pts = av_rescale_q(pts, inVideo->time_base, enc->time_base);
            else pts = nextPts;
            if (lastPts != AV_NOPTS_VALUE && pts <= lastPts) pts = lastPts + 1;
            lastPts = pts;
            nextPts = pts + std::max<int64_t>(1, av_rescale_q(1, av_inv_q(frameRate), enc->time_base));
            scaled->pts = pts;
            const double seconds = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                                       ? decoded->best_effort_timestamp * av_q2d(inVideo->time_base) - startTime
                                       : static_cast<double>(frames) / av_q2d(frameRate);
            av_frame_unref(decoded.get());

            drainEncoder(scaled.get());
            ++frames;
            if (frames % 8 == 0) {
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                const double fraction = duration > 0 ? std::clamp(seconds / duration, 0.0, 1.0) : 0.0;
                update([&](Progress& p) {
                    p.frames = frames;
                    p.elapsedS = elapsed;
                    p.fps = elapsed > 0 ? frames / elapsed : 0;
                    p.fraction = fraction;
                    p.etaS = fraction > 0.01 ? elapsed * (1.0 - fraction) / fraction : 0.0;
                });
            }
        }
    };

    bv::log::info("export: escribiendo {}", options_.output);
    while (!cancel_ && !writeFailed && !filterFailed && av_read_frame(in.get(), packet.get()) >= 0) {
        if (packet->stream_index == vIndex) {
            if (avcodec_send_packet(dec.get(), packet.get()) >= 0) encodeDecoded();
        } else if (outAudio && packet->stream_index == aIndex) {
            av_packet_rescale_ts(packet.get(), in->streams[aIndex]->time_base, outAudio->time_base);
            packet->stream_index = outAudio->index;
            packet->pos = -1;
            if (av_interleaved_write_frame(out.get(), packet.get()) < 0) writeFailed = true;
        }
        av_packet_unref(packet.get());
    }

    bv::log::info("export: fin de la entrada tras {} fotogramas", frames);
    if (!cancel_ && !writeFailed && !filterFailed) {
        avcodec_send_packet(dec.get(), nullptr);
        encodeDecoded();
        drainEncoder(nullptr);
        if (av_write_trailer(out.get()) < 0) writeFailed = true;
        bv::log::info("export: cerrado ({} fotogramas)", frames);
    }
    out.reset(); // closes the file before a possible delete

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (cancel_ || writeFailed || filterFailed) {
        std::error_code ec;
        std::filesystem::remove(pathFromUtf8(options_.output), ec);
        if (cancel_) {
            update([&](Progress& p) {
                p.state = State::Cancelled;
                p.elapsedS = elapsed;
            });
            return;
        }
        return fail(filterFailed ? "DLSS NR falló al procesar un fotograma" : "error al escribir (¿disco lleno?)");
    }
    update([&](Progress& p) {
        p.state = frames > 0 ? State::Done : State::Failed;
        p.frames = frames;
        p.elapsedS = elapsed;
        p.fps = elapsed > 0 ? frames / elapsed : 0;
        p.fraction = frames > 0 ? 1.0 : p.fraction;
        p.etaS = 0;
        p.message = frames > 0 ? note : "no se decodificó ningún fotograma";
    });
}

} // namespace bv::exporter
