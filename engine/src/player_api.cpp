// C ABI for the player (see bv_api.h). Windows only: it needs the D3D12 presenter.
#include "blackvolt/bv_api.h"

#include "audio/WasapiSink.h"
#include "media/CaptureDevices.h"
#include "media/CaptureUrl.h"
#include "core/EngineHandle.h"
#include "player/Player.h"
#include "render/D3D12Presenter.h"
#include "render/EnhanceConvert.h"

#include <algorithm>
#include <cstring>
#include <memory>


struct bv_player {
#if defined(_WIN32)
    std::unique_ptr<bv::render::D3D12Presenter> presenter;
#endif
    std::unique_ptr<bv::player::Player> player;
};

namespace {
template <std::size_t N>
void copyString(char (&dst)[N], const std::string& src) {
    const auto n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}
} // namespace

extern "C" {

bv_result BV_CALL bv_player_create(bv_engine* engine, bv_player** out_player) {
    if (!engine || !out_player) return BV_ERR_INVALID_ARGUMENT;
    *out_player = nullptr;
#if defined(_WIN32)
    try {
        auto p = std::make_unique<bv_player>();
        p->presenter = bv::render::D3D12Presenter::create(
            engine->engine.gpu(), engine->engine.ngx(), &engine->engine.telemetry(), engine->engine.nrLocator());
        if (!p->presenter) return BV_ERR_INTERNAL;
        p->player = std::make_unique<bv::player::Player>(engine->engine.gpu().device(), *p->presenter,
                                                         [] { return bv::audio::createWasapiSink(); },
                                                         engine->engine.telemetry());
        *out_player = p.release();
        return BV_OK;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

void BV_CALL bv_player_destroy(bv_player* player) {
    if (!player) return;
    player->player.reset(); // stop threads before the presenter goes away
    delete player;
}

bv_result BV_CALL bv_player_swapchain(bv_player* player, void** out_swapchain) {
    if (!player || !out_swapchain) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    *out_swapchain = player->presenter->swapChain();
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_player_resize(bv_player* player, uint32_t width, uint32_t height, float scale_x, float scale_y) {
    if (!player) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    player->presenter->resize(width, height, scale_x, scale_y);
    return BV_OK;
#else
    (void)width;
    (void)height;
    (void)scale_x;
    (void)scale_y;
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_player_open(bv_player* player, const char* url) {
    if (!player || !url) return BV_ERR_INVALID_ARGUMENT;
    player->player->open(url);
    return BV_OK;
}

bv_result BV_CALL bv_player_play(bv_player* player) {
    if (!player) return BV_ERR_INVALID_ARGUMENT;
    player->player->play();
    return BV_OK;
}

bv_result BV_CALL bv_player_pause(bv_player* player) {
    if (!player) return BV_ERR_INVALID_ARGUMENT;
    player->player->pause();
    return BV_OK;
}

bv_result BV_CALL bv_player_seek(bv_player* player, double seconds) {
    if (!player) return BV_ERR_INVALID_ARGUMENT;
    player->player->seek(seconds);
    return BV_OK;
}

bv_result BV_CALL bv_player_set_volume(bv_player* player, float volume) {
    if (!player) return BV_ERR_INVALID_ARGUMENT;
    player->player->setVolume(volume);
    return BV_OK;
}

bv_result BV_CALL bv_player_tracks(bv_player* player, bv_track* out, uint32_t capacity, uint32_t* count) {
    if (!player || !count) return BV_ERR_INVALID_ARGUMENT;
    const auto s = player->player->snapshot();
    *count = static_cast<uint32_t>(s.tracks.size());
    for (uint32_t i = 0; out && i < capacity && i < s.tracks.size(); ++i) {
        const auto& t = s.tracks[i];
        bv_track& o = out[i];
        o = bv_track{};
        o.size = sizeof(o);
        o.index = t.index;
        o.kind = static_cast<uint32_t>(t.kind);
        copyString(o.language, t.language);
        copyString(o.title, t.title);
        copyString(o.codec, t.codec);
    }
    return BV_OK;
}

bv_result BV_CALL bv_player_select_track(bv_player* player, uint32_t kind, int32_t index) {
    if (!player || kind > 1) return BV_ERR_INVALID_ARGUMENT;
    if (kind == 0) player->player->selectAudioTrack(index);
    else player->player->selectSubtitleTrack(index);
    return BV_OK;
}

bv_result BV_CALL bv_player_subtitle(bv_player* player, char* out, uint32_t size) {
    if (!player || !out || size == 0) return BV_ERR_INVALID_ARGUMENT;
    const auto text = player->player->subtitleNow();
    const auto n = std::min<std::size_t>(text.size(), size - 1);
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
    return BV_OK;
}

bv_result BV_CALL bv_player_set_rate(bv_player* player, double rate) {
    if (!player || !(rate > 0)) return BV_ERR_INVALID_ARGUMENT;
    player->player->setRate(rate);
    return BV_OK;
}

bv_result BV_CALL bv_player_info_get(bv_player* player, bv_player_info* out) {
    if (!player || !out) return BV_ERR_INVALID_ARGUMENT;
    const auto s = player->player->snapshot();
    bv_player_info i{};
    i.size = sizeof(i);
    i.state = static_cast<bv_player_state>(s.state);
    i.position_s = s.position;
    i.duration_s = s.duration;
    i.width = static_cast<uint32_t>(s.width);
    i.height = static_cast<uint32_t>(s.height);
    i.fps = s.fps;
    i.hardware_decode = s.hardwareDecode ? 1u : 0u;
    i.has_audio = s.hasAudio ? 1u : 0u;
    i.frames_presented = s.framesPresented;
    i.frames_dropped = s.framesDropped;
    i.frames_generated = s.framesGenerated;
    i.audio_track = s.audioTrack;
    i.subtitle_track = s.subtitleTrack;
    copyString(i.codec, s.codec);
    copyString(i.error, s.error);
    i.live = s.live ? 1u : 0u;
    *out = i;
    return BV_OK;
}

bv_result BV_CALL bv_capture_devices(bv_capture_device* out, uint32_t capacity, uint32_t* count) {
    if (!count || (capacity && !out)) return BV_ERR_INVALID_ARGUMENT;
    try {
        const auto devices = bv::media::enumerateCaptureDevices();
        *count = static_cast<uint32_t>(devices.size());
        for (uint32_t i = 0; i < capacity && i < devices.size(); ++i) {
            bv_capture_device d{};
            d.size = sizeof(d);
            d.is_audio = devices[i].audio ? 1u : 0u;
            copyString(d.name, devices[i].name);
            copyString(d.id, devices[i].id);
            out[i] = d;
        }
        return BV_OK;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
}

bv_result BV_CALL bv_capture_url(const char* video, const char* audio, uint32_t width, uint32_t height, uint32_t fps,
                                 char* out, uint32_t out_size) {
    if (!video || !*video || !out || out_size == 0) return BV_ERR_INVALID_ARGUMENT;
    const auto url = bv::media::makeCaptureUrl(video, audio ? audio : "", static_cast<int>(width),
                                               static_cast<int>(height), static_cast<int>(fps));
    if (url.size() + 1 > out_size) return BV_ERR_INVALID_ARGUMENT;
    std::memcpy(out, url.c_str(), url.size() + 1);
    return BV_OK;
}

bv_result BV_CALL bv_player_set_enhance(bv_player* player, const bv_enhance_settings* settings) {
    if (!player || !settings || settings->size < sizeof(bv_enhance_settings)) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    player->presenter->setEnhance(bv::render::fromApi(*settings));
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_player_screenshot(bv_player* player, const char* png_path) {
    if (!player || !png_path) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    try {
        return player->presenter->screenshot(png_path) ? BV_OK : BV_ERR_INTERNAL;
    } catch (...) {
        return BV_ERR_INTERNAL;
    }
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

bv_result BV_CALL bv_player_enhance_status(bv_player* player, bv_enhance_status* out) {
    if (!player || !out || out->size < sizeof(bv_enhance_status)) return BV_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    bv::render::toApi(player->presenter->enhanceStatus(), *out);
    return BV_OK;
#else
    return BV_ERR_UNSUPPORTED;
#endif
}

uint32_t BV_CALL bv_display_hdr(float* peak_nits) { return bv::render::displayHdrInfo(peak_nits) ? 1u : 0u; }

} // extern "C"
