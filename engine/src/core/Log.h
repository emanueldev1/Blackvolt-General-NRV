#pragma once

#include "blackvolt/bv_api.h"

#include <format>
#include <string>
#include <string_view>

namespace bv::log {

void setSink(bv_log_fn fn, void* user);
void write(bv_log_level level, std::string_view message);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(BV_LOG_INFO, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(BV_LOG_WARN, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(BV_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace bv::log
