#include "check.h"
#include "telemetry/RollingStats.h"
#include "telemetry/Telemetry.h"

using bv::telemetry::RollingStats;

TEST(empty_window_reports_zero) {
    RollingStats<8> s;
    const auto snap = s.snapshot();
    CHECK(snap.samples == 0);
    CHECK(snap.max == 0.0);
}

TEST(percentiles_over_1_to_100) {
    RollingStats<128> s;
    for (int i = 1; i <= 100; ++i) s.record(i);
    const auto snap = s.snapshot();
    CHECK(snap.samples == 100);
    CHECK_NEAR(snap.avg, 50.5, 1e-9);
    CHECK_NEAR(snap.p50, 50.0, 1e-9);
    CHECK_NEAR(snap.p95, 95.0, 1e-9);
    CHECK_NEAR(snap.p99, 99.0, 1e-9);
    CHECK_NEAR(snap.max, 100.0, 1e-9);
}

TEST(window_keeps_only_most_recent) {
    RollingStats<4> s;
    for (int i = 1; i <= 10; ++i) s.record(i); // keeps 7,8,9,10
    const auto snap = s.snapshot();
    CHECK(snap.samples == 4);
    CHECK_NEAR(snap.avg, 8.5, 1e-9);
    CHECK_NEAR(snap.max, 10.0, 1e-9);
}

TEST(registry_creates_and_finds_stages) {
    bv::telemetry::Telemetry t;
    CHECK(t.find("decode") == nullptr);
    t.stage("decode").record(1200.0);
    const auto* found = t.find("decode");
    CHECK(found != nullptr);
    CHECK(found && found->snapshot().samples == 1);
    t.resetAll();
    CHECK(found && found->snapshot().samples == 0);
}
