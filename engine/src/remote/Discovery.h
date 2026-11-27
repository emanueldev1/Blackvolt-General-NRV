#pragma once

#include "ConsoleProfile.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bv::remote {

enum class ConsoleState { Unknown, Ready, Standby };

struct DiscoveredConsole {
    std::string host;          // IP address that answered
    std::string name;          // console name shown in its settings
    std::string hostId;        // console id (MAC), lowercase hex
    std::string systemVersion;
    std::string runningApp;    // title currently running, if any
    bool ps5 = false;
    ConsoleState state = ConsoleState::Unknown;
};

// LAN discovery: broadcasts on every interface (not only 255.255.255.255, which Windows
// sends out of a single adapter) and pings known consoles directly, so consoles on
// another subnet or over a VPN still show up.
class Discovery {
public:
    Discovery();
    ~Discovery();
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // knownHosts: addresses of paired consoles to ping by unicast as well.
    bool start(const std::vector<std::string>& knownHosts, std::string* error = nullptr);
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::vector<DiscoveredConsole> consoles() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// IPv4 broadcast address of every up, non-loopback interface (host byte order strings).
std::vector<std::string> interfaceBroadcastAddresses();

// Sends the Remote Play wake-up packet. Needs the console's address and its regist key.
bool wakeConsole(const ConsoleProfile& console, std::string* error = nullptr);

// The wake credential is the regist key's leading hex digits (up to 8, NUL-terminated).
bool wakeCredential(const ConsoleProfile& console, uint64_t& credential);

} // namespace bv::remote
