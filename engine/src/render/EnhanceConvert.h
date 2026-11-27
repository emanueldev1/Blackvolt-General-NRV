#pragma once

// Conversions between the C ABI enhancement structs and the presenter's types.
#include "blackvolt/bv_api.h"
#include "render/D3D12Presenter.h"

namespace bv::render {

inline EnhanceSettings fromApi(const bv_enhance_settings& in) {
    EnhanceSettings s;
    s.vsr = in.vsr != 0;
    s.vsrQuality = in.vsr_quality ? in.vsr_quality : 4;
    s.trueHdr = in.true_hdr != 0;
    s.hdr.contrast = in.hdr_contrast;
    s.hdr.saturation = in.hdr_saturation;
    s.hdr.middleGray = in.hdr_middle_gray ? in.hdr_middle_gray : 50;
    s.hdr.peakNits = in.hdr_peak_nits;
    return s;
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
}

} // namespace bv::render
