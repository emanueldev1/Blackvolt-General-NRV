#include "check.h"

#include "blackvolt/bv_api.h"
#include "export/Exporter.h"
#include "media/Demuxer.h"
#include "media/FFmpeg.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace bv::exporter;

namespace {
const std::string kClip = std::string(BV_SOURCE_DIR) + "/tests/data/clip_2s.mp4";

std::string tempFile(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

Progress runToEnd(Job& job) {
    for (int i = 0; i < 600; ++i) {
        const auto p = job.progress();
        if (p.state != State::Running) return p;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    job.cancel();
    job.wait();
    return job.progress();
}
} // namespace

TEST(export_fit_size_keeps_aspect_and_even) {
    uint32_t w = 0, h = 0;
    fitSize(1920, 1080, 0, 0, w, h);
    CHECK(w == 1920 && h == 1080);
    fitSize(1920, 1080, 0, 2160, w, h);
    CHECK(w == 3840 && h == 2160);
    fitSize(1920, 1080, 1280, 0, w, h);
    CHECK(w == 1280 && h == 720);
    fitSize(1920, 800, 1920, 1080, w, h); // letterboxed source fits by width
    CHECK(w == 1920 && h == 800);
    fitSize(853, 481, 0, 0, w, h);
    CHECK(w % 2 == 0 && h % 2 == 0);
}

TEST(export_default_encoders_prefer_hardware) {
    const auto hevc = defaultEncoders(Codec::Hevc);
    CHECK(!hevc.empty() && hevc.front() == "hevc_nvenc");
    CHECK(defaultEncoders(Codec::Av1).front() == "av1_nvenc");
    CHECK(defaultEncoders(Codec::H264).back() == "mpeg4"); // always-built safety net
}

TEST(export_transcodes_and_scales_clip) {
    const auto output = tempFile("bv_export_test.mkv");
    std::filesystem::remove(output);
    Job job;
    Options o;
    o.input = kClip;
    o.output = output;
    o.codec = Codec::H264;
    o.height = 360;
    av_log_set_level(AV_LOG_VERBOSE); // diagnóstico si algo se atasca en CI
    // Software encoders only, so the test runs anywhere.
    o.encoders = {"libx264", "mpeg4"};
    job.start(o);
    const auto p = runToEnd(job);
    av_log_set_level(AV_LOG_INFO);
    if (p.state != State::Done) std::printf("  export: %s\n", p.message.c_str());
    CHECK(p.state == State::Done);
    CHECK(p.frames > 10);
    CHECK(p.outHeight == 360);
    CHECK(!p.hardware);

    std::printf("  export: comprobando %s\n", output.c_str());
    std::string error;
    auto d = bv::media::Demuxer::open(output, &error);
    std::printf("  export: abierto=%d %s\n", d.has_value() ? 1 : 0, error.c_str());
    CHECK(d.has_value());
    if (d) {
        CHECK(d->video().params->height == 360);
        CHECK(d->durationSeconds() > 1.5);
    }
    std::filesystem::remove(output);
}

TEST(export_cancel_removes_output) {
    const auto output = tempFile("bv_export_cancel.mp4");
    Job job;
    Options o;
    o.input = kClip;
    o.output = output;
    o.encoders = {"libx264", "mpeg4"};
    job.start(o);
    job.cancel();
    job.wait();
    const auto p = job.progress();
    CHECK(p.state == State::Cancelled || p.state == State::Done);
    if (p.state == State::Cancelled) CHECK(!std::filesystem::exists(output));
    std::filesystem::remove(output);
}

TEST(export_c_api_reports_errors) {
    bv_export_options o{};
    o.size = sizeof(o);
    o.input = "/definitely/not/here.mp4";
    o.output = "/tmp/never.mp4";
    o.codec = BV_EXPORT_HEVC;
    o.quality = 70;
    bv_export* job = nullptr;
    CHECK(bv_export_start(&o, &job) == BV_OK);
    bv_export_status s{};
    s.size = sizeof(s);
    for (int i = 0; i < 100; ++i) {
        CHECK(bv_export_status_get(job, &s) == BV_OK);
        if (s.state != BV_EXPORT_RUNNING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(s.state == BV_EXPORT_FAILED);
    CHECK(s.message[0] != '\0');
    bv_export_destroy(job);
    CHECK(bv_export_has_encoder("mpeg4") == 1);
    CHECK(bv_export_has_encoder("definitely_not_an_encoder") == 0);
    o.size = 4;
    CHECK(bv_export_start(&o, &job) == BV_ERR_INVALID_ARGUMENT);
}
