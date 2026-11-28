#pragma once

// Conversions between the C ABI enhancement structs and the presenter's types.
#include "blackvolt/bv_api.h"
#include "render/D3D12Presenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bv::render {

namespace detail {
inline float clampf(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
} // namespace detail

inline EnhanceSettings fromApi(const bv_enhance_settings& in) {
    using detail::clampf;
    EnhanceSettings s;
    s.vsr = in.vsr != 0;
    s.vsrQuality = in.vsr_quality ? in.vsr_quality : 4;
    s.trueHdr = in.true_hdr != 0;
    s.hdr.contrast = in.hdr_contrast;
    s.hdr.saturation = in.hdr_saturation;
    s.hdr.middleGray = in.hdr_middle_gray ? in.hdr_middle_gray : 50;
    s.hdr.peakNits = in.hdr_peak_nits;
    s.neural = in.neural != 0;

    auto& n = s.neuralControls;
    n.variant = in.nr_runtime <= BV_NR_ORIGINAL ? static_cast<ngx::NrVariant>(in.nr_runtime) : ngx::NrVariant::Auto;
    n.performanceModel = in.nr_performance_model != 0;
    n.size = (in.nr_size <= 4 || (in.nr_size >= 125 && in.nr_size <= 200)) ? in.nr_size
                                                                           : static_cast<uint32_t>(enhance::NrSize::P1080);
    n.style = std::clamp(in.nr_style, 0, 2);
    n.intensity = static_cast<float>(std::min<uint32_t>(in.neural_intensity, 100)) / 100.0f;
    n.tone = clampf(in.nr_tone, 0, 1, 1);
    n.structure = clampf(in.nr_structure, 0, 1, 1);
    n.skin = std::isfinite(in.nr_skin) && in.nr_skin >= 0 ? std::min(in.nr_skin, 2.0f) : -1.0f;
    n.autoMask = in.nr_auto_mask != 0;
    n.uiCorrection = in.nr_ui_correction != 0;
    n.total = clampf(in.nr_total, 0, 5, 1);
    n.darken = clampf(in.nr_darken, 0, 2, 1);
    n.brighten = clampf(in.nr_brighten, 0, 2, 1);
    n.color = clampf(in.nr_color, 0, 2, 1);
    n.luminance = clampf(in.nr_luminance, 0, 2, 1);
    n.correction = in.nr_correction != 0;
    n.correctionAuto = in.nr_correction_auto != 0;
    n.correctionAmount = clampf(in.nr_correction_amount, 0, 1, 1);
    n.hue = clampf(in.nr_hue, 0, 1, 1);
    n.chroma = clampf(in.nr_chroma, 0, 1, 0.75f);
    n.highlight = clampf(in.nr_highlight, 0, 1, 1);
    n.compression = clampf(in.nr_compression, 0, 1, 0.75f);
    n.neutral = clampf(in.nr_neutral, 0, 1, 0);
    n.colorKeep = clampf(in.nr_color_keep, 0, 1, 0);
    n.lumaKeep = clampf(in.nr_luma_keep, 0, 1, 0);
    n.shadow = clampf(in.nr_shadow, 0, 1, 0);
    n.protect = in.nr_protect <= BV_NR_PROTECT_CENTER ? static_cast<enhance::NrProtect>(in.nr_protect)
                                                      : enhance::NrProtect::None;
    n.feather = clampf(in.nr_feather, 0, 64, 8);
    n.antiFlicker = in.nr_anti_flicker <= 4 ? in.nr_anti_flicker : 0;
    n.stability = clampf(in.nr_stability, 0, 1, 1);
    s.interpolation = std::clamp<uint32_t>(in.interpolation, 1, 4);
    s.compare = in.compare > 2 ? 1u : in.compare;
    s.denoise = std::min<uint32_t>(in.denoise, 3);
    s.scaler = in.scaler == 1 ? 1u : 0u;
    s.sharpness = clampf(in.sharpness, 0, 1, 0.25f);
    s.color.brightness = clampf(in.brightness, -1, 1, 0);
    s.color.contrast = clampf(in.contrast, 0, 2, 1);
    s.color.saturation = clampf(in.saturation, 0, 2, 1);
    s.color.vibrance = clampf(in.vibrance, -1, 1, 0);
    s.color.temperature = clampf(in.temperature, -1, 1, 0);
    s.color.tint = clampf(in.tint, -1, 1, 0);
    s.color.gamma = in.gamma > 0 ? clampf(in.gamma, 0.5f, 2, 1) : 1.0f;
    s.color.fxaa = in.fxaa != 0;
    s.hdrPassthrough = in.hdr_tonemap == 0;
    return s;
}

template <std::size_t N>
inline void copyText(char (&dst)[N], const std::string& src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

inline void toApi(const EnhanceStatus& in, bv_enhance_status& out) {
    out.flags = in.flags;
    out.input_width = in.inputWidth;
    out.input_height = in.inputHeight;
    out.output_width = in.outputWidth;
    out.output_height = in.outputHeight;
    out.display_peak_nits = in.displayPeakNits;
    out.gpu_convert_ms = in.gpuConvertMs;
    out.gpu_vsr_ms = in.gpuVsrMs;
    out.gpu_hdr_ms = in.gpuHdrMs;
    out.gpu_total_ms = in.gpuTotalMs;
    out.gpu_neural_ms = in.gpuNeuralMs;
    copyText(out.neural_note, in.neuralNote);
    out.nr_width = in.nrWidth;
    out.nr_height = in.nrHeight;
    out.gpu_nr_prepare_ms = in.gpuNrPrepareMs;
    out.gpu_nr_composite_ms = in.gpuNrCompositeMs;
    out.nr_variant = static_cast<uint32_t>(in.nrVariant);
    copyText(out.nr_label, in.nrLabel);
    out.gpu_present_ms = in.gpuPresentMs;
    out.gpu_interp_ms = in.gpuInterpMs;
    out.interpolated_frames = in.interpolatedFrames;
}

} // namespace bv::render
