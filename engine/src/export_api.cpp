// C ABI for offline export (see bv_api.h). Platform independent.
#include "blackvolt/bv_api.h"

#include "core/EngineHandle.h"
#include "export/Exporter.h"
#include "export/GpuEnhancer.h"
#include "media/FFmpeg.h"
#include "render/EnhanceConvert.h"

#include <algorithm>
#include <cstring>
#include <memory>

struct bv_export {
    bv::exporter::Job job;
};

namespace {
template <std::size_t N>
void copyString(char (&dst)[N], const std::string& src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}
} // namespace

extern "C" {

bv_result BV_CALL bv_export_start(const bv_export_options* options, bv_export** out_job) {
    if (!options || !out_job || options->size < sizeof(bv_export_options) || !options->input || !options->output)
        return BV_ERR_INVALID_ARGUMENT;
    *out_job = nullptr;
    if (options->codec < BV_EXPORT_H264 || options->codec > BV_EXPORT_AV1) return BV_ERR_INVALID_ARGUMENT;
    try {
        bv::exporter::Options o;
        o.input = options->input;
        o.output = options->output;
        o.codec = static_cast<bv::exporter::Codec>(options->codec);
        o.width = options->width;
        o.height = options->height;
        o.quality = std::min<uint32_t>(options->quality, 100);
        o.audio = options->audio != 0;
        if (options->engine && options->enhance && options->enhance->size >= sizeof(bv_enhance_settings) &&
            (options->enhance->neural || options->enhance->denoise ||
             !bv::render::fromApi(*options->enhance).color.identity())) {
            // The job keeps a pointer to the engine: the caller must not destroy it while exporting.
            auto* engine = options->engine;
            const auto s = bv::render::fromApi(*options->enhance);
            o.filter = [engine, s](uint32_t w, uint32_t h, double frameMs, std::string* error) {
                return bv::exporter::createNrFilter(engine->engine.gpu().device(), engine->engine.ngx(),
                                                    engine->engine.nrLocator(), w, h, s.neural, s.neuralControls,
                                                    s.denoise, s.color, frameMs, error);
            };
        }
        auto job = std::make_unique<bv_export>();
        job->job.start(std::move(o));
        *out_job = job.release();
        return BV_OK;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
}

bv_result BV_CALL bv_export_status_get(bv_export* job, bv_export_status* out) {
    if (!job || !out || out->size < sizeof(bv_export_status)) return BV_ERR_INVALID_ARGUMENT;
    const auto p = job->job.progress();
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(bv_export_status));
    out->size = size;
    out->state = static_cast<bv_export_state>(p.state);
    out->progress = p.fraction;
    out->fps = p.fps;
    out->elapsed_s = p.elapsedS;
    out->eta_s = p.etaS;
    out->frames = p.frames;
    out->hardware = p.hardware ? 1u : 0u;
    out->width = p.outWidth;
    out->height = p.outHeight;
    copyString(out->encoder, p.encoder);
    copyString(out->message, p.message);
    copyString(out->enhance, p.enhance);
    return BV_OK;
}

bv_result BV_CALL bv_export_cancel(bv_export* job) {
    if (!job) return BV_ERR_INVALID_ARGUMENT;
    job->job.cancel();
    return BV_OK;
}

void BV_CALL bv_export_destroy(bv_export* job) {
    delete job; // the Job destructor cancels and joins
}

uint32_t BV_CALL bv_export_has_encoder(const char* name) {
    return name && avcodec_find_encoder_by_name(name) ? 1u : 0u;
}

} // extern "C"
