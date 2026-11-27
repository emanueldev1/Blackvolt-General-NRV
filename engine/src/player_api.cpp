// C ABI for the player (see bv_api.h). Windows only: it needs the D3D12 presenter.
#include "blackvolt/bv_api.h"

#include "audio/WasapiSink.h"
#include "core/EngineHandle.h"
#include "player/Player.h"
#include "render/D3D12Presenter.h"

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
        p->presenter = bv::render::D3D12Presenter::create(engine->engine.gpu());
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
    copyString(i.codec, s.codec);
    copyString(i.error, s.error);
    *out = i;
    return BV_OK;
}

} // extern "C"
