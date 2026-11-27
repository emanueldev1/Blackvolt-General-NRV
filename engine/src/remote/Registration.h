#pragma once

#include "ConsoleProfile.h"

#include <array>
#include <memory>
#include <string>

namespace bv::remote {

struct RegistrationRequest {
    std::string host;                    // console IP (from discovery or typed)
    ConsoleKind kind = ConsoleKind::PS5;
    std::array<uint8_t, 8> psnAccountId{}; // base64 "AQID..." decoded
    uint32_t pin = 0;                    // 8 digits shown on the console
    std::string nickname;                // optional, defaults to the console's name
};

// Pairs Blackvolt with a console (the PIN screen in Remote Play settings). Runs in the
// background; poll state().
class Registration {
public:
    enum class State { Running, Succeeded, Failed, Canceled };

    Registration();
    ~Registration(); // cancels and joins

    bool start(const RegistrationRequest& request, std::string* error = nullptr);
    void cancel();
    [[nodiscard]] State state() const;
    // Valid once state() == Succeeded.
    [[nodiscard]] ConsoleProfile result() const;
    [[nodiscard]] std::string error() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Parses the PSN account id as shown by tools (base64 of 8 bytes) or 16 hex digits.
bool parseAccountId(std::string_view text, std::array<uint8_t, 8>& out);

} // namespace bv::remote
