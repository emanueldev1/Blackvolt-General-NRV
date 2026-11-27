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
