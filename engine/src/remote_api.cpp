// C ABI for PlayStation Remote Play (see bv_api.h).
#include "blackvolt/bv_api.h"

#include "core/EngineHandle.h"
#include "core/Log.h"
#include "remote/ChiakiImport.h"
#include "remote/ConsoleStore.h"
#include "remote/Discovery.h"
#include "remote/Registration.h"
#include "remote/SdlGamepad.h"
#include "remote/StreamSession.h"
#include "remote/VeyraImport.h"
#include "runtime/Paths.h"

#if defined(_WIN32)
#include "audio/WasapiSink.h"
#include "render/D3D12Presenter.h"
#include "render/EnhanceConvert.h"
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>

using namespace bv::remote;

struct bv_consoles {
    explicit bv_consoles(std::filesystem::path p) : store(std::move(p)) {}
    std::mutex mutex;
    ConsoleStore store;
};

struct bv_discovery {
    Discovery discovery;
    std::vector<ConsoleProfile> known;
};

struct bv_pairing {
    bv_consoles* consoles = nullptr;
    Registration registration;
    bool stored = false;
};

struct bv_stream {
#if defined(_WIN32)
    std::unique_ptr<bv::render::D3D12Presenter> presenter;
#endif
    std::unique_ptr<StreamSession> session;
};

namespace {

template <std::size_t N>
void copyString(char (&dst)[N], const std::string& src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void copyError(char* dst, uint32_t size, const std::string& msg) {
    if (!dst || size == 0) return;
    const auto n = std::min<std::size_t>(msg.size(), size - 1);
    std::memcpy(dst, msg.data(), n);
    dst[n] = '\0';
}

template <class Fn>
bv_result guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        bv::log::error("remoteplay api: {}", e.what());
        return BV_ERR_INTERNAL;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
}

uint32_t importAndSave(bv_consoles* c, const std::vector<ConsoleProfile>& found) {
    std::lock_guard lock(c->mutex);
    const auto added = c->store.import(found);
    c->store.save(); // also persists refreshed keys/addresses of known consoles
    return static_cast<uint32_t>(added);
}

} // namespace

extern "C" {

// ---- consoles ----

bv_result BV_CALL bv_consoles_open(const char* path, bv_consoles** out) {
    if (!path || !out) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        auto c = std::make_unique<bv_consoles>(bv::runtime::fromUtf8(path));
        c->store.load(); // missing or unreadable = empty
        *out = c.release();
        return BV_OK;
    });
}

void BV_CALL bv_consoles_close(bv_consoles* consoles) { delete consoles; }

uint32_t BV_CALL bv_consoles_count(bv_consoles* consoles) {
    if (!consoles) return 0;
    std::lock_guard lock(consoles->mutex);
    return static_cast<uint32_t>(consoles->store.consoles().size());
}

bv_result BV_CALL bv_consoles_get(bv_consoles* consoles, uint32_t index, bv_console_info* out) {
    if (!consoles || !out) return BV_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(consoles->mutex);
    const auto& list = consoles->store.consoles();
    if (index >= list.size()) return BV_ERR_INVALID_ARGUMENT;
    const auto& p = list[index];
    bv_console_info i{};
    i.size = sizeof(i);
    copyString(i.nickname, p.nickname);
    copyString(i.host, p.host);
    if (p.hasMac) copyString(i.console_id, p.macHex());
    i.kind = p.kind == ConsoleKind::PS5 ? BV_CONSOLE_PS5 : BV_CONSOLE_PS4;
    copyString(i.source, p.source);
    i.has_account_id = p.hasAccountId ? 1u : 0u;
    if (p.prefs) {
        i.pref_width = p.prefs->width;
        i.pref_height = p.prefs->height;
        i.pref_fps = p.prefs->fps;
        i.pref_bitrate_kbps = p.prefs->bitrateKbps;
        i.pref_codec = p.prefs->codec;
    }
    *out = i;
    return BV_OK;
}

bv_result BV_CALL bv_consoles_remove(bv_consoles* consoles, uint32_t index) {
    if (!consoles) return BV_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(consoles->mutex);
    if (!consoles->store.remove(index)) return BV_ERR_INVALID_ARGUMENT;
    return consoles->store.save() ? BV_OK : BV_ERR_INTERNAL;
}

bv_result BV_CALL bv_consoles_update(bv_consoles* consoles, uint32_t index, const char* nickname, const char* host) {
    if (!consoles) return BV_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(consoles->mutex);
    auto& list = consoles->store.consoles();
    if (index >= list.size()) return BV_ERR_INVALID_ARGUMENT;
    if (nickname) list[index].nickname = nickname;
    if (host) list[index].host = host;
    return consoles->store.save() ? BV_OK : BV_ERR_INTERNAL;
}

bv_result BV_CALL bv_consoles_import_installed(bv_consoles* consoles, uint32_t* added) {
    if (!consoles) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        std::vector<ConsoleProfile> found = chiaki::fromRegistry();
        mergeProfiles(found, veyra::fromDirectory(veyra::defaultDirectory()));
        const auto n = importAndSave(consoles, found);
        if (added) *added = n;
        bv::log::info("remoteplay: imported {} new console(s) from chiaki-ng/Veyra", n);
        return BV_OK;
    });
}

bv_result BV_CALL bv_consoles_import_path(bv_consoles* consoles, const char* path, uint32_t* added) {
    if (!consoles || !path) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        const auto p = bv::runtime::fromUtf8(path);
        std::error_code ec;
        std::vector<ConsoleProfile> found;
        if (std::filesystem::is_directory(p, ec)) {
            found = veyra::fromDirectory(p);
            // A Veyra install folder: look in its remoteplay subfolder too.
            if (found.empty()) found = veyra::fromDirectory(p / "remoteplay");
        } else {
            found = chiaki::fromIniFile(p);
        }
        const auto n = importAndSave(consoles, found);
        if (added) *added = n;
        return found.empty() ? BV_ERR_UNSUPPORTED : BV_OK;
    });
}

bv_result BV_CALL bv_consoles_wake(bv_consoles* consoles, uint32_t index, char* error, uint32_t error_size) {
    if (!consoles) return BV_ERR_INVALID_ARGUMENT;
    ConsoleProfile p;
    {
        std::lock_guard lock(consoles->mutex);
        if (index >= consoles->store.consoles().size()) return BV_ERR_INVALID_ARGUMENT;
        p = consoles->store.consoles()[index];
    }
    std::string err;
    if (!wakeConsole(p, &err)) {
        copyError(error, error_size, err);
        return BV_ERR_UNSUPPORTED;
    }
    return BV_OK;
}

// ---- discovery ----

bv_result BV_CALL bv_discovery_start(bv_consoles* known, bv_discovery** out) {
    if (!out) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        auto d = std::make_unique<bv_discovery>();
        std::vector<std::string> hosts;
        if (known) {
            std::lock_guard lock(known->mutex);
            d->known = known->store.consoles();
        }
        for (const auto& k : d->known) {
            if (!k.host.empty()) hosts.push_back(k.host);
        }
        std::string err;
        if (!d->discovery.start(hosts, &err)) {
            bv::log::error("remoteplay: {}", err);
            return BV_ERR_INTERNAL;
        }
        *out = d.release();
        return BV_OK;
    });
}

void BV_CALL bv_discovery_stop(bv_discovery* discovery) { delete discovery; }

bv_result BV_CALL bv_discovery_list(bv_discovery* discovery, bv_discovered_console* items, uint32_t capacity,
                                    uint32_t* count) {
    if (!discovery || !count || (capacity && !items)) return BV_ERR_INVALID_ARGUMENT;
    const auto list = discovery->discovery.consoles();
    *count = static_cast<uint32_t>(list.size());
    for (uint32_t i = 0; i < capacity && i < list.size(); ++i) {
        const auto& c = list[i];
        bv_discovered_console o{};
        o.size = sizeof(o);
        copyString(o.name, c.name);
        copyString(o.host, c.host);
        copyString(o.console_id, c.hostId);
        o.kind = c.ps5 ? BV_CONSOLE_PS5 : BV_CONSOLE_PS4;
        o.state = c.state == ConsoleState::Ready ? BV_DISCOVERED_READY
                  : c.state == ConsoleState::Standby ? BV_DISCOVERED_STANDBY
                                                     : BV_DISCOVERED_UNKNOWN;
        copyString(o.running_app, c.runningApp);
        copyString(o.system_version, c.systemVersion);
        o.paired_index = -1;
        for (std::size_t k = 0; k < discovery->known.size(); ++k) {
            const auto& known = discovery->known[k];
            if ((known.hasMac && known.macHex() == c.hostId) || (!known.hasMac && known.host == c.host)) {
                o.paired_index = static_cast<int32_t>(k);
                break;
            }
        }
        items[i] = o;
    }
    return BV_OK;
}

// ---- pairing ----

bv_result BV_CALL bv_pairing_start(bv_consoles* consoles, const char* host, uint32_t kind, const char* account_id,
                                   uint32_t pin, bv_pairing** out) {
    if (!consoles || !host || !*host || !out || pin > 99999999u) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        RegistrationRequest req;
        req.host = host;
        req.kind = kind == BV_CONSOLE_PS4 ? ConsoleKind::PS4 : ConsoleKind::PS5;
        req.pin = pin;
        if (account_id && *account_id) {
            if (!parseAccountId(account_id, req.psnAccountId)) return BV_ERR_INVALID_ARGUMENT;
        } else {
            std::lock_guard lock(consoles->mutex);
            const auto& list = consoles->store.consoles();
            const auto it = std::find_if(list.begin(), list.end(), [](const ConsoleProfile& p) { return p.hasAccountId; });
            if (it == list.end()) return BV_ERR_INVALID_ARGUMENT;
            req.psnAccountId = it->psnAccountId;
        }
        auto p = std::make_unique<bv_pairing>();
        p->consoles = consoles;
        std::string err;
        if (!p->registration.start(req, &err)) {
            bv::log::error("remoteplay: {}", err);
            return BV_ERR_INTERNAL;
        }
        *out = p.release();
        return BV_OK;
    });
}

bv_result BV_CALL bv_pairing_poll(bv_pairing* pairing, uint32_t* state, char* error, uint32_t error_size) {
    if (!pairing || !state) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        switch (pairing->registration.state()) {
        case Registration::State::Running: *state = BV_PAIRING_RUNNING; break;
        case Registration::State::Succeeded:
            *state = BV_PAIRING_SUCCEEDED;
            if (!pairing->stored) {
                importAndSave(pairing->consoles, {pairing->registration.result()});
                pairing->stored = true;
            }
            break;
        case Registration::State::Canceled: *state = BV_PAIRING_CANCELED; break;
        default:
            *state = BV_PAIRING_FAILED;
            copyError(error, error_size, pairing->registration.error());
            break;
        }
        return BV_OK;
    });
}

void BV_CALL bv_pairing_destroy(bv_pairing* pairing) { delete pairing; }

// ---- streaming ----

bv_result BV_CALL bv_stream_create(bv_engine* engine, bv_stream** out) {
    if (!engine || !out) return BV_ERR_INVALID_ARGUMENT;
    *out = nullptr;
#if defined(_WIN32)
    return guarded([&] {
        auto s = std::make_unique<bv_stream>();
        s->presenter = bv::render::D3D12Presenter::create(
            engine->engine.gpu(), engine->engine.ngx(), &engine->engine.telemetry(), engine->engine.nrLocator());
        if (!s->presenter) return BV_ERR_INTERNAL;
        s->session = std::make_unique<StreamSession>(engine->engine.gpu().device(), *s->presenter,
                                                     [] { return bv::audio::createWasapiSink(); }, createSdlGamepad(),
                                                     engine->engine.telemetry());
        *out = s.release();
        return BV_OK;
    });
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

void BV_CALL bv_stream_destroy(bv_stream* stream) {
    if (!stream) return;
    stream->session.reset(); // threads stop before the presenter goes away
    delete stream;
}

bv_result BV_CALL bv_stream_swapchain(bv_stream* stream, void** out_swapchain) {
    if (!stream || !out_swapchain) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    *out_swapchain = stream->presenter->swapChain();
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_stream_resize(bv_stream* stream, uint32_t width, uint32_t height, float scale_x, float scale_y) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    stream->presenter->resize(width, height, scale_x, scale_y);
    return BV_OK;
#else
    (void)width;
    (void)height;
    (void)scale_x;
    (void)scale_y;
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_stream_start(bv_stream* stream, bv_consoles* consoles, uint32_t index,
                                  const bv_stream_settings* settings) {
    if (!stream || !consoles || !settings || settings->size < sizeof(bv_stream_settings)) return BV_ERR_INVALID_ARGUMENT;
    ConsoleProfile p;
    {
        std::lock_guard lock(consoles->mutex);
        if (index >= consoles->store.consoles().size()) return BV_ERR_INVALID_ARGUMENT;
        p = consoles->store.consoles()[index];
    }
    if (p.host.empty()) return BV_ERR_INVALID_ARGUMENT;
    StreamSettings s;
    s.width = settings->width;
    s.height = settings->height;
    s.fps = settings->fps == 30 ? 30 : 60;
    s.bitrateKbps = std::min<uint32_t>(settings->bitrate_kbps, 100000);
    s.codec = std::min<uint32_t>(settings->codec, 2);
    s.autoReconnect = settings->auto_reconnect != 0;
    s.wakeIfAsleep = settings->wake_if_asleep != 0;
    s.enableDualSense = settings->dualsense_features != 0;
    return guarded([&] {
        stream->session->start(p, s);
        return BV_OK;
    });
}

bv_result BV_CALL bv_stream_stop(bv_stream* stream) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
    stream->session->stop();
    return BV_OK;
}

bv_result BV_CALL bv_stream_info_get(bv_stream* stream, bv_stream_info* out) {
    if (!stream || !out) return BV_ERR_INVALID_ARGUMENT;
    const auto s = stream->session->snapshot();
    bv_stream_info i{};
    i.size = sizeof(i);
    i.state = static_cast<uint32_t>(s.state);
    i.width = s.width;
    i.height = s.height;
    i.fps = s.fps;
    i.bitrate_mbps = s.bitrateMbps;
    i.rtt_ms = s.rttMs;
    i.decode_ms = s.decodeMs;
    i.frames_presented = s.framesPresented;
    i.frames_lost = s.framesLost;
    i.frames_recovered = s.framesRecovered;
    i.reconnects = s.reconnects;
    i.hardware_decode = s.hardwareDecode ? 1u : 0u;
    i.gamepad_connected = s.gamepadConnected ? 1u : 0u;
    copyString(i.gamepad, s.gamepadName);
    copyString(i.codec, s.codec);
    copyString(i.error, s.error);
    *out = i;
    return BV_OK;
}

bv_result BV_CALL bv_stream_set_volume(bv_stream* stream, float volume) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
    stream->session->setVolume(volume);
    return BV_OK;
}

bv_result BV_CALL bv_stream_set_input(bv_stream* stream, const bv_gamepad_state* state) {
    if (!stream || !state) return BV_ERR_INVALID_ARGUMENT;
    GamepadState g;
    g.buttons = state->buttons;
    g.l2 = state->l2;
    g.r2 = state->r2;
    g.leftX = state->left_x;
    g.leftY = state->left_y;
    g.rightX = state->right_x;
    g.rightY = state->right_y;
    stream->session->setOverlayInput(g);
    return BV_OK;
}

bv_result BV_CALL bv_stream_ps_button(bv_stream* stream) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
    stream->session->sendPsButton();
    return BV_OK;
}

bv_result BV_CALL bv_stream_console_sleep(bv_stream* stream) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
    stream->session->putConsoleToSleep();
    return BV_OK;
}

bv_result BV_CALL bv_stream_request_keyframe(bv_stream* stream) {
    if (!stream) return BV_ERR_INVALID_ARGUMENT;
    stream->session->requestKeyframe();
    return BV_OK;
}

bv_result BV_CALL bv_stream_set_enhance(bv_stream* stream, const bv_enhance_settings* settings) {
    if (!stream || !settings || settings->size < sizeof(bv_enhance_settings)) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    stream->presenter->setEnhance(bv::render::fromApi(*settings));
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_stream_screenshot(bv_stream* stream, const char* png_path) {
    if (!stream || !png_path) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    try {
        return stream->presenter->screenshot(png_path) ? BV_OK : BV_ERR_INTERNAL;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_stream_enhance_status(bv_stream* stream, bv_enhance_status* out) {
    if (!stream || !out || out->size < sizeof(bv_enhance_status)) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    bv::render::toApi(stream->presenter->enhanceStatus(), *out);
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

} // extern "C"
