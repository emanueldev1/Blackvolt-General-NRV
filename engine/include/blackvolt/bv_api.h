/*
 * Blackvolt engine - public C ABI.
 *
 * The engine is a native DLL. Everything the UI needs goes through this header,
 * so the UI (C# / WinUI 3) never depends on C++ types or the engine's internals.
 *
 * Rules:
 *  - All functions are thread-safe unless stated otherwise.
 *  - Strings are UTF-8, owned by the caller unless stated otherwise.
 *  - Structs carry a `size` field set by the caller for forward compatibility.
 */
#ifndef BLACKVOLT_BV_API_H
#define BLACKVOLT_BV_API_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(BV_ENGINE_BUILD)
#    define BV_API __declspec(dllexport)
#  else
#    define BV_API __declspec(dllimport)
#  endif
#  define BV_CALL __cdecl
#else
#  define BV_API __attribute__((visibility("default")))
#  define BV_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BV_API_VERSION 5u

typedef enum bv_result {
    BV_OK = 0,
    BV_ERR_INVALID_ARGUMENT = 1,
    BV_ERR_NOT_INITIALIZED = 2,
    BV_ERR_NO_COMPATIBLE_GPU = 3,
    BV_ERR_DEVICE_LOST = 4,
    BV_ERR_UNSUPPORTED = 5,
    BV_ERR_INTERNAL = 99
} bv_result;

typedef enum bv_gpu_vendor {
    BV_VENDOR_UNKNOWN = 0,
    BV_VENDOR_NVIDIA = 1,
    BV_VENDOR_AMD = 2,
    BV_VENDOR_INTEL = 3
} bv_gpu_vendor;

/* Capability bits reported per adapter. */
enum {
    BV_CAP_D3D12 = 1u << 0,
    BV_CAP_RTX_VIDEO_SR = 1u << 1,  /* RTX Video Super Resolution */
    BV_CAP_RTX_TRUE_HDR = 1u << 2,  /* RTX Video TrueHDR */
    BV_CAP_NVENC = 1u << 3,
    BV_CAP_NVDEC = 1u << 4,
    BV_CAP_OPTICAL_FLOW = 1u << 5,  /* NVOF hardware (Turing+) */
    BV_CAP_TEARING = 1u << 6        /* Variable refresh / tearing present */
};

typedef struct bv_adapter_info {
    uint32_t size;                 /* = sizeof(bv_adapter_info) */
    uint32_t index;
    bv_gpu_vendor vendor;
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_vram_bytes;
    uint32_t capabilities;         /* BV_CAP_* */
    char name[128];                /* UTF-8 */
    char driver_version[32];       /* e.g. "566.36", empty if unknown */
} bv_adapter_info;

/* Rolling statistics over the last N frames of a pipeline stage. Times in microseconds. */
typedef struct bv_stage_stats {
    uint32_t size;                 /* = sizeof(bv_stage_stats) */
    uint32_t samples;
    double avg_us;
    double p50_us;
    double p95_us;
    double p99_us;
    double max_us;
} bv_stage_stats;

/* NVIDIA NGX features Blackvolt can drive. */
typedef enum bv_feature {
    BV_FEATURE_DLSS_SR = 0,      /* DLSS Super Resolution     nvngx_dlss.dll    */
    BV_FEATURE_DLSS_RR = 1,      /* DLSS Ray Reconstruction   nvngx_dlssd.dll   */
    BV_FEATURE_DLSS_FG = 2,      /* DLSS Frame Generation     nvngx_dlssg.dll   */
    BV_FEATURE_DLSS_NR = 3,      /* DLSS Neural Rendering     nvngx_dlssnr.dll  */
    BV_FEATURE_RTX_VSR = 4,      /* RTX Video Super Res.      nvngx_vsr.dll     */
    BV_FEATURE_RTX_TRUEHDR = 5,  /* RTX Video TrueHDR         nvngx_truehdr.dll */
    BV_FEATURE_COUNT = 6
} bv_feature;

typedef enum bv_runtime_state {
    BV_RUNTIME_MISSING = 0,        /* DLL not found in any runtime folder */
    BV_RUNTIME_FOUND = 1,          /* DLL present; GPU support not checked yet */
    BV_RUNTIME_READY = 2,          /* DLL present and NGX reports the GPU supports it */
    BV_RUNTIME_UNSUPPORTED_GPU = 3,
    BV_RUNTIME_DRIVER_TOO_OLD = 4,
    BV_RUNTIME_NGX_UNAVAILABLE = 5 /* engine built without NGX or NGX init failed */
} bv_runtime_state;

typedef enum bv_runtime_origin {
    BV_ORIGIN_UNKNOWN = 0,         /* hash not in the manifest */
    BV_ORIGIN_OFFICIAL = 1,        /* unmodified NVIDIA build */
    BV_ORIGIN_COMMUNITY = 2        /* community-modified build */
} bv_runtime_origin;

typedef struct bv_runtime_status {
    uint32_t size;                 /* = sizeof(bv_runtime_status) */
    bv_feature feature;
    bv_runtime_state state;
    bv_runtime_origin origin;
    char label[96];                /* e.g. "DLSS 310.9.16 (NVIDIA)", empty if missing */
    char path[520];                /* UTF-8 absolute path of the DLL in use */
} bv_runtime_status;

/* Where runtime DLLs live. Must be called before bv_engine_create.
 * bundled_dir: read-only folder shipped with the app (Release package).
 * user_dir:    writable folder for imported runtimes (e.g. %LOCALAPPDATA%\Blackvolt\runtime).
 * manifest:    path to runtime/manifest.json. */
BV_API bv_result BV_CALL bv_set_runtime_paths(const char* bundled_dir, const char* user_dir, const char* manifest);

/* Copies a runtime DLL into user_dir after checking it is the right file for the feature.
 * out (optional) receives the resulting status. */
BV_API bv_result BV_CALL bv_runtime_import(const char* source_path, bv_feature feature, bv_runtime_status* out);

/* Searches a folder (e.g. a Veyra install) for runtime DLLs. Fills up to `capacity` paths
 * (UTF-8, 520 bytes each) and the feature each one provides. */
typedef struct bv_runtime_candidate {
    uint32_t size;
    bv_feature feature;
    bv_runtime_origin origin;
    char label[96];
    char path[520];
} bv_runtime_candidate;
BV_API bv_result BV_CALL bv_runtime_scan(const char* folder, bv_runtime_candidate* out, uint32_t capacity,
                                         uint32_t* count);

typedef struct bv_engine bv_engine;

typedef enum bv_log_level { BV_LOG_DEBUG = 0, BV_LOG_INFO, BV_LOG_WARN, BV_LOG_ERROR } bv_log_level;
typedef void(BV_CALL* bv_log_fn)(bv_log_level level, const char* message, void* user);

BV_API uint32_t BV_CALL bv_api_version(void);
BV_API const char* BV_CALL bv_result_string(bv_result r);

/* Adapter enumeration does not need an engine instance. */
BV_API bv_result BV_CALL bv_enum_adapters(bv_adapter_info* out, uint32_t capacity, uint32_t* count);

/* adapter_index = UINT32_MAX picks the best adapter (NVIDIA RTX first). */
BV_API bv_result BV_CALL bv_engine_create(uint32_t adapter_index, bv_engine** out_engine);
BV_API void BV_CALL bv_engine_destroy(bv_engine* engine);
BV_API bv_result BV_CALL bv_engine_adapter(bv_engine* engine, bv_adapter_info* out);
BV_API void BV_CALL bv_set_log_callback(bv_log_fn fn, void* user);

BV_API bv_result BV_CALL bv_engine_runtime_status(bv_engine* engine, bv_feature feature, bv_runtime_status* out);

/* Stage statistics by name: "decode", "enhance", "present", "frame" (end to end). */
BV_API bv_result BV_CALL bv_engine_stage_stats(bv_engine* engine, const char* stage, bv_stage_stats* out);

/* ------------------------------------------------------------------------------------------
 * Player
 * ---------------------------------------------------------------------------------------- */
typedef struct bv_player bv_player;

typedef enum bv_player_state {
    BV_PLAYER_IDLE = 0,
    BV_PLAYER_OPENING = 1,
    BV_PLAYER_PLAYING = 2,
    BV_PLAYER_PAUSED = 3,
    BV_PLAYER_ENDED = 4,
    BV_PLAYER_ERROR = 5
} bv_player_state;

typedef struct bv_player_info {
    uint32_t size;                 /* = sizeof(bv_player_info) */
    bv_player_state state;
    double position_s;
    double duration_s;
    uint32_t width;
    uint32_t height;
    double fps;
    uint32_t hardware_decode;      /* 1 = D3D12 Video decode */
    uint32_t has_audio;
    uint64_t frames_presented;
    uint64_t frames_dropped;
    char codec[32];
    char error[256];
} bv_player_info;

/* The player renders into a composition swapchain; attach it to a SwapChainPanel. */
BV_API bv_result BV_CALL bv_player_create(bv_engine* engine, bv_player** out_player);
BV_API void BV_CALL bv_player_destroy(bv_player* player);
BV_API bv_result BV_CALL bv_player_swapchain(bv_player* player, void** out_swapchain); /* IDXGISwapChain1*, not AddRef'd */
/* width/height in physical pixels; scale = panel CompositionScale (DPI), applied as the swapchain transform. */
BV_API bv_result BV_CALL bv_player_resize(bv_player* player, uint32_t width, uint32_t height, float scale_x, float scale_y);
BV_API bv_result BV_CALL bv_player_open(bv_player* player, const char* url);
BV_API bv_result BV_CALL bv_player_play(bv_player* player);
BV_API bv_result BV_CALL bv_player_pause(bv_player* player);
BV_API bv_result BV_CALL bv_player_seek(bv_player* player, double seconds);
BV_API bv_result BV_CALL bv_player_set_volume(bv_player* player, float volume); /* 0..2 */
BV_API bv_result BV_CALL bv_player_info_get(bv_player* player, bv_player_info* out);

/* ---------------------------------------------------------------------------
 * PlayStation Remote Play (PS5 / PS4)
 * ------------------------------------------------------------------------ */

typedef struct bv_consoles bv_consoles;   /* paired consoles, stored encrypted (DPAPI) */
typedef struct bv_discovery bv_discovery;
typedef struct bv_pairing bv_pairing;
typedef struct bv_stream bv_stream;

typedef enum bv_console_kind { BV_CONSOLE_PS4 = 0, BV_CONSOLE_PS5 = 1 } bv_console_kind;

typedef struct bv_console_info {
    uint32_t size;
    char nickname[64];
    char host[64];          /* last known address, may be empty */
    char console_id[16];    /* MAC as 12 hex digits, empty if unknown */
    uint32_t kind;          /* bv_console_kind */
    char source[16];        /* "blackvolt", "veyra", "chiaki-ng" */
    uint32_t has_account_id;
    uint32_t pref_width, pref_height, pref_fps, pref_bitrate_kbps, pref_codec; /* 0 when unknown */
} bv_console_info;

/* Opens (or creates) the console store. A missing file is an empty store. */
BV_API bv_result BV_CALL bv_consoles_open(const char* path, bv_consoles** out);
BV_API void BV_CALL bv_consoles_close(bv_consoles* consoles);
BV_API uint32_t BV_CALL bv_consoles_count(bv_consoles* consoles);
BV_API bv_result BV_CALL bv_consoles_get(bv_consoles* consoles, uint32_t index, bv_console_info* out);
BV_API bv_result BV_CALL bv_consoles_remove(bv_consoles* consoles, uint32_t index);
/* NULL leaves a field unchanged. */
BV_API bv_result BV_CALL bv_consoles_update(bv_consoles* consoles, uint32_t index, const char* nickname, const char* host);
/* Pairings saved by chiaki-ng (registry) and Veyra on this PC. */
BV_API bv_result BV_CALL bv_consoles_import_installed(bv_consoles* consoles, uint32_t* added);
/* A chiaki-ng settings export (.ini) or a folder with Veyra .dat files. */
BV_API bv_result BV_CALL bv_consoles_import_path(bv_consoles* consoles, const char* path, uint32_t* added);
BV_API bv_result BV_CALL bv_consoles_wake(bv_consoles* consoles, uint32_t index, char* error, uint32_t error_size);

typedef enum bv_discovered_state {
    BV_DISCOVERED_UNKNOWN = 0,
    BV_DISCOVERED_READY = 1,
    BV_DISCOVERED_STANDBY = 2
} bv_discovered_state;

typedef struct bv_discovered_console {
    uint32_t size;
    char name[64];
    char host[64];
    char console_id[16];
    uint32_t kind;          /* bv_console_kind */
    uint32_t state;         /* bv_discovered_state */
    char running_app[96];
    char system_version[32];
    int32_t paired_index;   /* index in the store, or -1 */
} bv_discovered_console;

/* known: optional; its consoles are also pinged directly (other subnets, VPN). */
BV_API bv_result BV_CALL bv_discovery_start(bv_consoles* known, bv_discovery** out);
BV_API void BV_CALL bv_discovery_stop(bv_discovery* discovery);
BV_API bv_result BV_CALL bv_discovery_list(bv_discovery* discovery, bv_discovered_console* items, uint32_t capacity,
                                           uint32_t* count);

typedef enum bv_pairing_state {
    BV_PAIRING_RUNNING = 0,
    BV_PAIRING_SUCCEEDED = 1,
    BV_PAIRING_FAILED = 2,
    BV_PAIRING_CANCELED = 3
} bv_pairing_state;

/* account_id: base64 (as chiaki-ng shows it), 16 hex digits or the decimal id; NULL/empty reuses
 * the account of an already paired console. pin: the 8 digits shown on the console. */
BV_API bv_result BV_CALL bv_pairing_start(bv_consoles* consoles, const char* host, uint32_t kind,
                                          const char* account_id, uint32_t pin, bv_pairing** out);
/* On success the console is added to the store (and saved) once. */
BV_API bv_result BV_CALL bv_pairing_poll(bv_pairing* pairing, uint32_t* state, char* error, uint32_t error_size);
BV_API void BV_CALL bv_pairing_destroy(bv_pairing* pairing);

typedef struct bv_stream_settings {
    uint32_t size;
    uint32_t width, height;     /* 640x360, 960x540, 1280x720, 1920x1080 */
    uint32_t fps;               /* 30 / 60 */
    uint32_t bitrate_kbps;      /* 0 = default for the resolution */
    uint32_t codec;             /* 0 H.264, 1 HEVC, 2 HEVC HDR */
    uint32_t auto_reconnect;
    uint32_t wake_if_asleep;
    uint32_t dualsense_features;
} bv_stream_settings;

typedef enum bv_stream_state {
    BV_STREAM_IDLE = 0,
    BV_STREAM_WAKING = 1,
    BV_STREAM_CONNECTING = 2,
    BV_STREAM_STREAMING = 3,
    BV_STREAM_RECONNECTING = 4,
    BV_STREAM_ENDED = 5,
    BV_STREAM_ERROR = 6
} bv_stream_state;

typedef struct bv_stream_info {
    uint32_t size;
    uint32_t state;             /* bv_stream_state */
    uint32_t width, height;
    double fps;
    double bitrate_mbps;
    double rtt_ms;
    double decode_ms;
    uint64_t frames_presented;
    uint64_t frames_lost;
    uint64_t frames_recovered;
    uint32_t reconnects;
    uint32_t hardware_decode;
    uint32_t gamepad_connected;
    char gamepad[64];
    char codec[32];
    char error[256];
} bv_stream_info;

typedef struct bv_gamepad_state {
    uint32_t size;
    uint32_t buttons;           /* cross=1<<0 moon=1<<1 box=1<<2 pyramid=1<<3 left right up down
                                   l1 r1 l3 r3 options share touchpad ps (1<<15) */
    uint8_t l2, r2;
    int16_t left_x, left_y, right_x, right_y;
} bv_gamepad_state;

BV_API bv_result BV_CALL bv_stream_create(bv_engine* engine, bv_stream** out);
BV_API void BV_CALL bv_stream_destroy(bv_stream* stream);
BV_API bv_result BV_CALL bv_stream_swapchain(bv_stream* stream, void** out_swapchain);
BV_API bv_result BV_CALL bv_stream_resize(bv_stream* stream, uint32_t width, uint32_t height, float scale_x, float scale_y);
BV_API bv_result BV_CALL bv_stream_start(bv_stream* stream, bv_consoles* consoles, uint32_t index,
                                         const bv_stream_settings* settings);
BV_API bv_result BV_CALL bv_stream_stop(bv_stream* stream);
BV_API bv_result BV_CALL bv_stream_info_get(bv_stream* stream, bv_stream_info* out);
BV_API bv_result BV_CALL bv_stream_set_volume(bv_stream* stream, float volume); /* 0..1 */
/* Keyboard/mouse mapped input, merged with the physical gamepad. */
BV_API bv_result BV_CALL bv_stream_set_input(bv_stream* stream, const bv_gamepad_state* state);
BV_API bv_result BV_CALL bv_stream_ps_button(bv_stream* stream);
BV_API bv_result BV_CALL bv_stream_console_sleep(bv_stream* stream);
/* Ask the console for a keyframe (clears artifacts after packet loss). */
BV_API bv_result BV_CALL bv_stream_request_keyframe(bv_stream* stream);

/* ------------------------------------------------------------------------------------------
 * Video enhancement (player and Remote Play stream share the same presenter)
 * ---------------------------------------------------------------------------------------- */
typedef struct bv_enhance_settings {
    uint32_t size;              /* = sizeof(bv_enhance_settings) */
    uint32_t vsr;               /* 1 = RTX Video Super Resolution to the on-screen size */
    uint32_t vsr_quality;       /* 1 (fastest) .. 4 (best) */
    uint32_t true_hdr;          /* 1 = RTX Video TrueHDR (SDR -> HDR, needs Windows HDR on) */
    uint32_t hdr_contrast;      /* 0..200, 100 = neutral */
    uint32_t hdr_saturation;    /* 0..200, 100 = neutral */
    uint32_t hdr_middle_gray;   /* 10..100 */
    uint32_t hdr_peak_nits;     /* 400..2000; 0 = the display's reported peak */
} bv_enhance_settings;

typedef enum bv_enhance_flags {
    BV_ENHANCE_VSR_ACTIVE = 1u << 0,
    BV_ENHANCE_HDR_ACTIVE = 1u << 1,
    BV_ENHANCE_VSR_UNAVAILABLE = 1u << 2, /* requested, but the GPU/driver/runtime refused */
    BV_ENHANCE_HDR_UNAVAILABLE = 1u << 3,
    BV_ENHANCE_DISPLAY_SDR = 1u << 4,     /* TrueHDR requested but Windows HDR is off */
    BV_ENHANCE_VSR_BYPASSED = 1u << 5     /* picture shown smaller than decoded: nothing to upscale */
} bv_enhance_flags;

typedef struct bv_enhance_status {
    uint32_t size;              /* = sizeof(bv_enhance_status) */
    uint32_t flags;             /* bv_enhance_flags */
    uint32_t input_width, input_height;
    uint32_t output_width, output_height;
    float display_peak_nits;
    /* Smoothed GPU time per stage, measured with timestamp queries. */
    float gpu_convert_ms, gpu_vsr_ms, gpu_hdr_ms, gpu_total_ms;
} bv_enhance_status;

BV_API bv_result BV_CALL bv_player_set_enhance(bv_player* player, const bv_enhance_settings* settings);
BV_API bv_result BV_CALL bv_player_enhance_status(bv_player* player, bv_enhance_status* out);
BV_API bv_result BV_CALL bv_stream_set_enhance(bv_stream* stream, const bv_enhance_settings* settings);
BV_API bv_result BV_CALL bv_stream_enhance_status(bv_stream* stream, bv_enhance_status* out);
/* 1 when any display is in HDR mode; peak_nits receives the highest reported peak (may be NULL). */
BV_API uint32_t BV_CALL bv_display_hdr(float* peak_nits);

#ifdef __cplusplus
}
#endif

#endif /* BLACKVOLT_BV_API_H */
