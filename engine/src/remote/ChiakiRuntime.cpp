#include "ChiakiRuntime.h"

#include "core/Log.h"

#include <chiaki/common.h>

#include <mutex>

namespace bv::remote {
namespace {

void forward(ChiakiLogLevel level, const char* msg, void*) {
    if (!msg) return;
    switch (level) {
    case CHIAKI_LOG_ERROR: log::error("remoteplay: {}", msg); break;
    case CHIAKI_LOG_WARNING: log::warn("remoteplay: {}", msg); break;
    default: log::info("remoteplay: {}", msg); break;
    }
}

ChiakiLog gLog{CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, forward, nullptr};
std::once_flag gOnce;
ChiakiErrorCode gInit = CHIAKI_ERR_UNINITIALIZED;

} // namespace

bool ensureChiakiInitialized(std::string* error) {
    std::call_once(gOnce, [] { gInit = chiaki_lib_init(); });
    if (gInit != CHIAKI_ERR_SUCCESS && error) *error = std::string("chiaki_lib_init: ") + chiaki_error_string(gInit);
    return gInit == CHIAKI_ERR_SUCCESS;
}

ChiakiLog* chiakiLog() { return &gLog; }

} // namespace bv::remote
