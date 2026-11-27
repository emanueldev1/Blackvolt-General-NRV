#include "Registration.h"

#include "Base64.h"
#include "ChiakiRuntime.h"
#include "Protect.h"
#include "core/Log.h"

#include <chiaki/regist.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace bv::remote {

struct Registration::Impl {
    ChiakiRegist regist{};
    bool started = false;
    std::atomic<State> state{State::Failed};
    mutable std::mutex mutex;
    ConsoleProfile profile;
    std::string error;
    RegistrationRequest request;

    static void onEvent(ChiakiRegistEvent* event, void* user) {
        auto* self = static_cast<Impl*>(user);
        std::lock_guard lock(self->mutex);
        switch (event->type) {
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS: {
            const ChiakiRegisteredHost& h = *event->registered_host;
            ConsoleProfile p;
            p.source = "blackvolt";
            p.host = self->request.host;
            p.kind = chiaki_target_is_ps5(h.target) ? ConsoleKind::PS5 : ConsoleKind::PS4;
            p.target = static_cast<uint32_t>(h.target);
            std::memcpy(p.mac.data(), h.server_mac, 6);
            p.hasMac = true;
            std::memcpy(p.registKey.data(), h.rp_regist_key, 16);
            std::memcpy(p.rpKey.data(), h.rp_key, 16);
            p.rpKeyType = h.rp_key_type;
            p.psnAccountId = self->request.psnAccountId;
            p.hasAccountId = true;
            p.nickname = !self->request.nickname.empty()
                             ? self->request.nickname
                             : std::string(h.server_nickname, strnlen(h.server_nickname, sizeof(h.server_nickname)));
            self->profile = std::move(p);
            self->state = State::Succeeded;
            log::info("remoteplay: paired with {}", self->profile.nickname);
            break;
        }
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
            self->state = State::Canceled;
            break;
        default:
            self->error = "la consola rechazó el emparejamiento (revisa el PIN y el ID de cuenta)";
            self->state = State::Failed;
            break;
        }
    }
};

Registration::Registration() : impl_(std::make_unique<Impl>()) {}

Registration::~Registration() {
    cancel();
    if (impl_->started) chiaki_regist_fini(&impl_->regist);
    secureWipe(impl_->profile.registKey.data(), impl_->profile.registKey.size());
    secureWipe(impl_->profile.rpKey.data(), impl_->profile.rpKey.size());
}

bool Registration::start(const RegistrationRequest& request, std::string* error) {
    if (impl_->started) {
        if (error) *error = "ya hay un emparejamiento en curso";
        return false;
    }
    if (request.host.empty()) {
        if (error) *error = "falta la dirección de la consola";
        return false;
    }
    if (!ensureChiakiInitialized(error)) return false;
    impl_->request = request;
    impl_->state = State::Running;

    ChiakiRegistInfo info{};
    info.target = request.kind == ConsoleKind::PS5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
    info.host = request.host.c_str(); // copied by chiaki_regist_start
    info.broadcast = false;
    info.psn_online_id = nullptr;     // account id based registration (PS4 7.0+ and PS5)
    std::memcpy(info.psn_account_id, request.psnAccountId.data(), 8);
    info.pin = request.pin;
    info.console_pin = 0;
    info.holepunch_info = nullptr;
    info.rudp = nullptr;

    const auto err = chiaki_regist_start(&impl_->regist, chiakiLog(), &info, &Impl::onEvent, impl_.get());
    if (err != CHIAKI_ERR_SUCCESS) {
        impl_->state = State::Failed;
        impl_->error = std::string("regist: ") + chiaki_error_string(err);
        if (error) *error = impl_->error;
        return false;
    }
    impl_->started = true;
    return true;
}

void Registration::cancel() {
    if (impl_->started && impl_->state == State::Running) chiaki_regist_stop(&impl_->regist);
}

Registration::State Registration::state() const { return impl_->state; }

ConsoleProfile Registration::result() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->profile;
}

std::string Registration::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

bool parseAccountId(std::string_view text, std::array<uint8_t, 8>& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.size() == 16) {
        bool hex = true;
        std::array<uint8_t, 8> v{};
        for (std::size_t i = 0; i < 16 && hex; ++i) {
            const char c = text[i];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) hex = false;
            else v[i / 2] = static_cast<uint8_t>(v[i / 2] << 4 | d);
        }
        if (hex) {
            out = v;
            return true;
        }
    }
    // Decimal account id (what PSN's API returns): stored little-endian, like chiaki-ng.
    if (!text.empty() && text.size() != 16 && text.size() <= 20 &&
        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        unsigned long long v = 0;
        for (char c : text) {
            const unsigned long long next = v * 10 + static_cast<unsigned>(c - '0');
            if (next / 10 != v) return false; // overflow
            v = next;
        }
        for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        return true;
    }
    const auto decoded = base64Decode(text);
    if (!decoded || decoded->size() != 8) return false;
    std::memcpy(out.data(), decoded->data(), 8);
    return true;
}

} // namespace bv::remote
