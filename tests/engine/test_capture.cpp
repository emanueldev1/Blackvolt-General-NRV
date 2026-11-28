#include "check.h"

#include "blackvolt/bv_api.h"
#include "media/CaptureUrl.h"

#include <cstring>

using namespace bv::media;

namespace {
const std::string* option(const CaptureUrl& c, const char* key) {
    for (const auto& [k, v] : c.options)
        if (k == key) return &v;
    return nullptr;
}
} // namespace

TEST(capture_url_round_trip) {
    const auto url = makeCaptureUrl("Elgato 4K60 Pro", "Elgato Audio", 1920, 1080, 60);
    CHECK(url == "capture:video=Elgato 4K60 Pro:audio=Elgato Audio?size=1920x1080&fps=60");
    CHECK(isCaptureUrl(url));
    const auto c = parseCaptureUrl(url);
    CHECK(c.has_value());
    if (!c) return;
    CHECK(option(*c, "video_size") && *option(*c, "video_size") == "1920x1080");
    CHECK(option(*c, "framerate") && *option(*c, "framerate") == "60");
#if defined(_WIN32)
    CHECK(c->inputFormat == "dshow");
    CHECK(c->target == "video=Elgato 4K60 Pro:audio=Elgato Audio");
#else
    CHECK(c->inputFormat == "v4l2");
    CHECK(c->target == "Elgato 4K60 Pro");
#endif
}

TEST(screen_capture_urls) {
    CHECK(isCaptureUrl("screen:desktop"));
#if defined(_WIN32)
    auto c = parseCaptureUrl("screen:desktop?fps=30&region=1920,0,2560,1441&cursor=0");
    CHECK(c.has_value());
    if (c) {
        CHECK(c->inputFormat == "gdigrab");
        CHECK(c->target == "desktop");
        CHECK(option(*c, "offset_x") && *option(*c, "offset_x") == "1920");
        CHECK(option(*c, "video_size") && *option(*c, "video_size") == "2560x1440"); // even sizes for the encoder
        CHECK(option(*c, "framerate") && *option(*c, "framerate") == "30");
        CHECK(option(*c, "draw_mouse") && *option(*c, "draw_mouse") == "0");
    }
    c = parseCaptureUrl("screen:title=¿Qué? - Bloc de notas?fps=60");
    CHECK(c && c->target == "title=¿Qué? - Bloc de notas");
    CHECK(c && option(*c, "framerate") && *option(*c, "framerate") == "60");
    CHECK(!parseCaptureUrl("screen:nothing").has_value());
    CHECK(!parseCaptureUrl("screen:desktop?fps=999").has_value());
#endif
}

TEST(capture_url_defaults_and_errors) {
    CHECK(makeCaptureUrl("Cam", "", 0, 0, 0) == "capture:video=Cam");
    CHECK(parseCaptureUrl("capture:video=Cam").has_value());
    CHECK(!isCaptureUrl("C:/videos/a.mkv"));
    std::string error;
    CHECK(!parseCaptureUrl("capture:audio=Mic", &error).has_value());
    CHECK(!error.empty());
    CHECK(!parseCaptureUrl("capture:video=Cam?size=axb").has_value());
    CHECK(!parseCaptureUrl("capture:video=Cam?fps=0").has_value());
    CHECK(!parseCaptureUrl("capture:video=Cam?format=bogus").has_value());
    const auto m = parseCaptureUrl("capture:video=Cam?format=mjpeg");
    CHECK(m && (option(*m, "vcodec") || option(*m, "input_format")));
}

TEST(capture_c_api) {
    char buf[256];
    CHECK(bv_capture_url("Cam", nullptr, 1280, 720, 30, buf, sizeof(buf)) == BV_OK);
    CHECK(std::strcmp(buf, "capture:video=Cam?size=1280x720&fps=30") == 0);
    CHECK(bv_capture_url("Cam", nullptr, 0, 0, 0, buf, 4) == BV_ERR_INVALID_ARGUMENT);
    CHECK(bv_capture_url("", nullptr, 0, 0, 0, buf, sizeof(buf)) == BV_ERR_INVALID_ARGUMENT);
    uint32_t count = 0;
    CHECK(bv_capture_devices(nullptr, 0, &count) == BV_OK);
}

#include "media/SubtitleDecoder.h"

TEST(ass_events_become_plain_text) {
    using bv::media::assToPlain;
    CHECK(assToPlain("0,0,Default,,0,0,0,,Hola {\\i1}mundo{\\i0}\\Nsegunda linea") == "Hola mundo\nsegunda linea");
    CHECK(assToPlain("12,0,Top,Name,10,10,10,fx,a,b,c") == "a,b,c"); // commas in the text stay
    CHECK(assToPlain("{\\an8}Arriba") == "Arriba");                    // bare text, no event fields
    CHECK(assToPlain("0,0,Default,,0,0,0,,  \\N ") == "");
    CHECK(assToPlain("0,0,Default,,0,0,0,,uno\\hdos") == "uno dos");
}
