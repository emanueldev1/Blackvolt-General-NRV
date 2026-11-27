#include "Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace bv::log {
namespace {
std::mutex g_mutex;
bv_log_fn g_fn = nullptr;
void* g_user = nullptr;
} // namespace

void setSink(bv_log_fn fn, void* user) {
    std::lock_guard lock(g_mutex);
    g_fn = fn;
    g_user = user;
}

void write(bv_log_level level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    const std::string text(message);
    if (g_fn) {
        g_fn(level, text.c_str(), g_user);
        return;
    }
    static constexpr const char* kNames[] = {"debug", "info", "warn", "error"};
    std::fprintf(stderr, "[blackvolt:%s] %s\n", kNames[level], text.c_str());
}

} // namespace bv::log
