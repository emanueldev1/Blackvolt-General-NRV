#pragma once

#include <array>

namespace bv::render {

enum class YuvMatrix { Bt601, Bt709, Bt2020 };

// Row-major 3x4: rgb = M * (y, cb, cr, 1). Handles limited (16-235) or full range.
// Pure math so it is unit tested on every platform.
inline std::array<float, 12> yuvToRgb(YuvMatrix m, bool fullRange) {
    float kr, kb;
    switch (m) {
    case YuvMatrix::Bt601: kr = 0.299f; kb = 0.114f; break;
    case YuvMatrix::Bt2020: kr = 0.2627f; kb = 0.0593f; break;
    default: kr = 0.2126f; kb = 0.0722f; break;
    }
    const float kg = 1.0f - kr - kb;
    const float ys = fullRange ? 1.0f : 255.0f / 219.0f;
    const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
    const float yo = fullRange ? 0.0f : -16.0f / 255.0f;
    const float co = -128.0f / 255.0f;

    const float rv = 2.0f * (1.0f - kr) * cs;
    const float gu = -2.0f * kb * (1.0f - kb) / kg * cs;
    const float gv = -2.0f * kr * (1.0f - kr) / kg * cs;
    const float bu = 2.0f * (1.0f - kb) * cs;

    return {
        ys, 0.0f, rv, ys * yo + rv * co,
        ys, gu, gv, ys * yo + (gu + gv) * co,
        ys, bu, 0.0f, ys * yo + bu * co,
    };
}

struct Viewport { float x, y, width, height; };

// Largest rectangle with the video's aspect ratio centred in the target (letterbox / pillarbox).
inline Viewport fitAspect(float videoW, float videoH, float sar, float targetW, float targetH) {
    if (videoW <= 0 || videoH <= 0 || targetW <= 0 || targetH <= 0) return {0, 0, targetW, targetH};
    const float aspect = (videoW * (sar > 0 ? sar : 1.0f)) / videoH;
    float w = targetW, h = targetW / aspect;
    if (h > targetH) {
        h = targetH;
        w = targetH * aspect;
    }
    return {(targetW - w) * 0.5f, (targetH - h) * 0.5f, w, h};
}

} // namespace bv::render
