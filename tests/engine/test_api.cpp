#include "check.h"
#include "blackvolt/bv_api.h"

TEST(api_version_matches_header) { CHECK(bv_api_version() == BV_API_VERSION); }

TEST(null_arguments_are_rejected) {
    CHECK(bv_enum_adapters(nullptr, 0, nullptr) == BV_ERR_INVALID_ARGUMENT);
    CHECK(bv_engine_create(0, nullptr) == BV_ERR_INVALID_ARGUMENT);
    bv_stage_stats s{};
    CHECK(bv_engine_stage_stats(nullptr, "frame", &s) == BV_ERR_INVALID_ARGUMENT);
}

TEST(enum_adapters_reports_count) {
    uint32_t count = 12345;
    CHECK(bv_enum_adapters(nullptr, 0, &count) == BV_OK);
    CHECK(count != 12345u);
}
