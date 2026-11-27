#pragma once
// Minimal test harness: no external dependency needed to build the tests.
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace check {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Register { Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
}

#define BV_CONCAT_(a, b) a##b
#define BV_CONCAT(a, b) BV_CONCAT_(a, b)
#define TEST(name) \
    static void name(); \
    static check::Register BV_CONCAT(reg_, name)(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) { std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++check::failures(); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))
