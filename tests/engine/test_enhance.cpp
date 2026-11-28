#include "check.h"

#include "enhance/NeuralRendering.h"
#include "render/EnhanceConvert.h"

#include <cstring>

using namespace bv;

TEST(neural_rendering_needs_a_runtime) {
    std::string reason;
    auto stage = enhance::NeuralRendering::create(nullptr, nullptr, nullptr, {}, 1920, 1080, {}, &reason);
    CHECK(stage == nullptr);
    CHECK(!reason.empty());
}

TEST(nr_extent_keeps_aspect_and_never_upscales) {
    using enhance::NrSize;
    const auto u = [](NrSize s) { return static_cast<uint32_t>(s); };
    auto e = enhance::nrExtent(3840, 2160, u(NrSize::P1080));
    CHECK(e[0] == 1920 && e[1] == 1080);
    e = enhance::nrExtent(1280, 720, u(NrSize::P1080));
    CHECK(e[0] == 1280 && e[1] == 720);
    e = enhance::nrExtent(3840, 2160, u(NrSize::Native));
    CHECK(e[0] == 3840 && e[1] == 2160);
    e = enhance::nrExtent(1920, 1080, 150); // 50 %
    CHECK(e[0] == 960 && e[1] == 540);
    e = enhance::nrExtent(3840, 2160, u(NrSize::P720));
    CHECK(e[0] == 1280 && e[1] == 720);
    CHECK(e[0] % 2 == 0 && e[1] % 2 == 0);
}

TEST(nr_architecture_policy_matches_veyra) {
    uint32_t arch = 0x172;
    CHECK(ngx::rewriteNrAmpereArchitecture(arch, true, 0) && arch == 0x1B0);
    arch = 0x192; // Ada stays Ada
    CHECK(!ngx::rewriteNrAmpereArchitecture(arch, true, 0) && arch == 0x192);
    arch = 0x172; // another adapter, or a failed query, is never touched
    CHECK(!ngx::rewriteNrAmpereArchitecture(arch, false, 0) && arch == 0x172);
    // SF 310.9.16.0 and 310.8.2+ pick their own route; 310.8.0.0 needs the rewrite.
    CHECK(!ngx::nrRuntimeNeedsAmpereRewrite((310u << 16) | 9u, 16u << 16));
    CHECK(!ngx::nrRuntimeNeedsAmpereRewrite((310u << 16) | 8u, 2u << 16));
    CHECK(ngx::nrRuntimeNeedsAmpereRewrite((310u << 16) | 8u, 0));
    CHECK(ngx::nrRenderPreset(ngx::NrVariant::Compat, true) == 2);
    CHECK(ngx::nrRenderPreset(ngx::NrVariant::Compat, false) == 1);
    CHECK(ngx::nrRenderPreset(ngx::NrVariant::Original, true) == 0);
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
    in.nr_runtime = 2;
    in.nr_size = 3;
    in.nr_style = 7;            // clamped to 2
    in.nr_skin = -5;            // unset
    in.nr_total = 9;            // clamped to 5
    in.nr_darken = 0.5f;
    in.nr_protect = 99;         // unknown -> none
    const auto s = render::fromApi(in);
    CHECK(s.vsr && s.trueHdr && s.neural);
    CHECK(s.vsrQuality == 4);
    CHECK(s.hdr.middleGray == 50);
    const auto& n = s.neuralControls;
    CHECK(n.intensity == 1.0f);
    CHECK(n.variant == ngx::NrVariant::Original);
    CHECK(n.size == 3);
    CHECK(n.style == 2);
    CHECK(n.skin == -1.0f);
    CHECK(n.total == 5.0f);
    CHECK(n.darken == 0.5f);
    CHECK(n.protect == enhance::NrProtect::None);
}

TEST(nr_feature_identity_ignores_live_controls) {
    enhance::NeuralControls a, b;
    b.style = 2;
    b.total = 3;
    CHECK(a.sameFeature(b));
    b.performanceModel = !a.performanceModel;
    CHECK(!a.sameFeature(b));
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
