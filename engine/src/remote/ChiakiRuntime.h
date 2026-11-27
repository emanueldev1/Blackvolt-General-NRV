#pragma once

#include <chiaki/log.h>

#include <string>

namespace bv::remote {

// One-time chiaki-lib initialization (sockets, crypto) and a log bridge into Blackvolt's log.
bool ensureChiakiInitialized(std::string* error = nullptr);
ChiakiLog* chiakiLog();

} // namespace bv::remote
