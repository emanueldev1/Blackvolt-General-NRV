#include "Discovery.h"

#include "ChiakiRuntime.h"
#include "core/Log.h"

#include <chiaki/discovery.h>
#include <chiaki/discoveryservice.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace bv::remote {

struct Discovery::Impl {
    ChiakiDiscoveryService service{};
    bool running = false;
    mutable std::mutex mutex;
    std::vector<DiscoveredConsole> consoles;

    static void onHosts(ChiakiDiscoveryHost* hosts, size_t count, void* user) {
        auto* self = static_cast<Impl*>(user);
        std::vector<DiscoveredConsole> list;
        list.reserve(count);
        auto str = [](const char* s) { return s ? std::string(s) : std::string(); };
        for (size_t i = 0; i < count; ++i) {
            auto& h = hosts[i];
            DiscoveredConsole c;
            c.host = str(h.host_addr);
            c.name = str(h.host_name);
            c.hostId = str(h.host_id);
            std::transform(c.hostId.begin(), c.hostId.end(), c.hostId.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            c.systemVersion = str(h.system_version);
            c.runningApp = str(h.running_app_name);
            c.ps5 = chiaki_discovery_host_is_ps5(&h);
            c.state = h.state == CHIAKI_DISCOVERY_HOST_STATE_READY     ? ConsoleState::Ready
                      : h.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY ? ConsoleState::Standby
                                                                       : ConsoleState::Unknown;
            list.push_back(std::move(c));
        }
        std::lock_guard lock(self->mutex);
        self->consoles = std::move(list);
    }
};

Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}
Discovery::~Discovery() { stop(); }

bool Discovery::start(const std::vector<std::string>& knownHosts, std::string* error) {
    stop();
    if (!ensureChiakiInitialized(error)) return false;

    std::vector<sockaddr_storage> targets;
    auto add = [&](const std::string& ip) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip.c_str(), &in.sin_addr) != 1) return;
        const bool dup = std::any_of(targets.begin(), targets.end(), [&](const sockaddr_storage& s) {
            return reinterpret_cast<const sockaddr_in&>(s).sin_addr.s_addr == in.sin_addr.s_addr;
        });
        if (dup) return;
        sockaddr_storage st{};
        std::memcpy(&st, &in, sizeof(in));
        targets.push_back(st);
    };
    for (const auto& b : interfaceBroadcastAddresses()) add(b);
    for (const auto& h : knownHosts) add(h); // unicast pings to paired consoles

    sockaddr_in all{};
    all.sin_family = AF_INET;
    all.sin_addr.s_addr = 0xffffffff;
    sockaddr_storage sendAddr{};
    std::memcpy(&sendAddr, &all, sizeof(all));

    ChiakiDiscoveryServiceOptions options{};
    options.ping_ms = 500;
    options.ping_initial_ms = 50;
    options.hosts_max = 32;
    options.host_drop_pings = 6; // ~3 s without an answer and the console disappears
    options.send_addr = &sendAddr;
    options.send_addr_size = sizeof(sockaddr_in);
    options.broadcast_addrs = targets.empty() ? nullptr : targets.data();
    options.broadcast_num = targets.size();
    options.cb = &Impl::onHosts;
    options.cb_user = impl_.get();

    const auto err = chiaki_discovery_service_init(&impl_->service, &options, chiakiLog());
    if (err != CHIAKI_ERR_SUCCESS) {
        if (error) *error = std::string("discovery: ") + chiaki_error_string(err);
        return false;
    }
    impl_->running = true;
    log::info("remoteplay: discovery on {} address(es)", targets.size() + 1);
    return true;
}

void Discovery::stop() {
    if (!impl_->running) return;
    chiaki_discovery_service_fini(&impl_->service);
    impl_->running = false;
    std::lock_guard lock(impl_->mutex);
    impl_->consoles.clear();
}

bool Discovery::running() const { return impl_->running; }

std::vector<DiscoveredConsole> Discovery::consoles() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->consoles;
}

std::vector<std::string> interfaceBroadcastAddresses() {
    std::vector<std::string> out;
    auto push = [&](uint32_t addrNet, uint32_t maskNet) {
        const uint32_t bcast = addrNet | ~maskNet;
        in_addr a{};
        a.s_addr = bcast;
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &a, buf, sizeof(buf)) && std::find(out.begin(), out.end(), buf) == out.end()) {
            out.emplace_back(buf);
        }
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength == 0 ||
                u->OnLinkPrefixLength > 30) {
                continue;
            }
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const uint32_t mask = htonl(0xffffffffu << (32 - u->OnLinkPrefixLength));
            push(in->sin_addr.s_addr, mask);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        push(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr,
             reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
    }
    freeifaddrs(list);
#endif
    return out;
}

bool wakeCredential(const ConsoleProfile& console, uint64_t& credential) {
    std::size_t len = 0;
    while (len < console.registKey.size() && console.registKey[len] != 0) ++len;
    if (len == 0 || len > 8) return false;
    uint64_t v = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(console.registKey[i]);
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + static_cast<uint64_t>(d);
    }
    credential = v;
    return true;
}

bool wakeConsole(const ConsoleProfile& console, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (console.host.empty()) return fail("la consola no tiene dirección conocida");
    uint64_t credential = 0;
    if (!wakeCredential(console, credential)) return fail("clave de registro no válida para despertar");
    if (!ensureChiakiInitialized(error)) return false;
    const auto err = chiaki_discovery_wakeup(chiakiLog(), nullptr, console.host.c_str(), credential,
                                             console.kind == ConsoleKind::PS5);
    if (err != CHIAKI_ERR_SUCCESS) return fail(std::string("wakeup: ") + chiaki_error_string(err));
    log::info("remoteplay: wake-up sent to {}", console.host);
    return true;
}

} // namespace bv::remote
