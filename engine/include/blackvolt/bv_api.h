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

#define BV_API_VERSION 2u

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

#ifdef __cplusplus
}
#endif

#endif /* BLACKVOLT_BV_API_H */
