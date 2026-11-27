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

#define BV_API_VERSION 1u

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

/* Stage statistics by name: "decode", "enhance", "present", "frame" (end to end). */
BV_API bv_result BV_CALL bv_engine_stage_stats(bv_engine* engine, const char* stage, bv_stage_stats* out);

#ifdef __cplusplus
}
#endif

#endif /* BLACKVOLT_BV_API_H */
