#include "check.h"

#include "enhance/NeuralRendering.h"
#include "render/EnhanceConvert.h"

#include <cstring>

using namespace bv;

TEST(neural_rendering_reports_unavailable_without_implementation) {
    std::string reason;
    auto stage = enhance::createNeuralRendering(nullptr, nullptr, 1920, 1080, &reason);
    CHECK(stage == nullptr);
    CHECK(!reason.empty());
}

TEST(enhance_settings_round_trip_from_c_api) {
    bv_enhance_settings in{};
    in.size = sizeof(in);
    in.vsr = 1;
    in.vsr_quality = 0;         // 0 = default (best)
    in.true_hdr = 1;
    in.hdr_middle_gray = 0;     // 0 = default
    in.neural = 1;
    in.neural_intensity = 250;  // clamped to 100
    in.neural_temporal = 1;
    const auto s = render::fromApi(in);
    CHECK(s.vsr && s.trueHdr && s.neural);
    CHECK(s.vsrQuality == 4);
    CHECK(s.hdr.middleGray == 50);
    CHECK(s.neuralControls.intensity == 1.0f);
    CHECK(s.neuralControls.temporal);
}

TEST(enhance_status_copies_note_with_terminator) {
    render::EnhanceStatus st;
    st.flags = render::EnhanceStatus::NeuralUnavailable;
    st.neuralNote = std::string(400, 'x');
    bv_enhance_status out{};
    out.size = sizeof(out);
    render::toApi(st, out);
    CHECK(out.flags == BV_ENHANCE_NEURAL_UNAVAILABLE);
    CHECK(std::strlen(out.neural_note) == sizeof(out.neural_note) - 1);
}

TEST(true_hdr_params_are_clamped) {
    ngx::TrueHdrParams p{500, 500, 1, 99999};
    const auto c = ngx::TrueHdr::clamp(p);
    CHECK(c.contrast == 200 && c.saturation == 200);
    CHECK(c.middleGray == 10);
    CHECK(c.peakNits == 2000);
}
