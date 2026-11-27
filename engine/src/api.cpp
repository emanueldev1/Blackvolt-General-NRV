#include "blackvolt/bv_api.h"

#include "core/Engine.h"
#include "core/Log.h"
#include "gpu/AdapterRanking.h"
#include "gpu/Adapters.h"
#include "runtime/Catalog.h"
#include "runtime/Paths.h"

#include <algorithm>
#include <cstring>

#include <cstdint>
#include <exception>
#include <new>

struct bv_engine {
    bv_engine(std::unique_ptr<bv::gpu::D3D12Context> gpu, bv::runtime::RuntimePaths paths)
        : engine(std::move(gpu), std::move(paths)) {}
    bv::Engine engine;
};

namespace {

// Exceptions must never cross the C ABI.
template <class Fn>
bv_result guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        bv::log::error("out of memory");
    } catch (const std::exception& e) {
        bv::log::error("unhandled exception: {}", e.what());
    } catch (...) {
        bv::log::error("unhandled unknown exception");
    }
    return BV_ERR_INTERNAL;
}

} // namespace

extern "C" {

uint32_t BV_CALL bv_api_version(void) { return BV_API_VERSION; }

const char* BV_CALL bv_result_string(bv_result r) {
    switch (r) {
    case BV_OK: return "ok";
    case BV_ERR_INVALID_ARGUMENT: return "invalid argument";
    case BV_ERR_NOT_INITIALIZED: return "not initialized";
    case BV_ERR_NO_COMPATIBLE_GPU: return "no compatible GPU";
    case BV_ERR_DEVICE_LOST: return "GPU device lost";
    case BV_ERR_UNSUPPORTED: return "unsupported";
    default: return "internal error";
    }
}

bv_result BV_CALL bv_enum_adapters(bv_adapter_info* out, uint32_t capacity, uint32_t* count) {
    if (!count) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        const auto adapters = bv::gpu::enumerateAdapters();
        *count = static_cast<uint32_t>(adapters.size());
        if (out) {
            for (uint32_t i = 0; i < capacity && i < adapters.size(); ++i) out[i] = adapters[i];
        }
        return BV_OK;
    });
}

bv_result BV_CALL bv_engine_create(uint32_t adapter_index, bv_engine** out_engine) {
    if (!out_engine) return BV_ERR_INVALID_ARGUMENT;
    *out_engine = nullptr;
    return guarded([&] {
        const auto adapters = bv::gpu::enumerateAdapters();
        if (adapters.empty()) return BV_ERR_NO_COMPATIBLE_GPU;

        const bv_adapter_info* chosen = nullptr;
        if (adapter_index == UINT32_MAX) {
            chosen = &adapters[static_cast<std::size_t>(bv::gpu::bestAdapter(adapters))];
        } else {
            for (const auto& a : adapters) {
                if (a.index == adapter_index) chosen = &a;
            }
        }
        if (!chosen) return BV_ERR_INVALID_ARGUMENT;

        bv_result err = BV_OK;
        auto gpu = bv::gpu::D3D12Context::create(*chosen, &err);
        if (!gpu) return err;
        *out_engine = new bv_engine{std::move(gpu), bv::runtime::globalPaths()};
        return BV_OK;
    });
}

void BV_CALL bv_engine_destroy(bv_engine* engine) { delete engine; }

bv_result BV_CALL bv_engine_adapter(bv_engine* engine, bv_adapter_info* out) {
    if (!engine || !out) return BV_ERR_INVALID_ARGUMENT;
    *out = engine->engine.gpu().adapter();
    return BV_OK;
}

void BV_CALL bv_set_log_callback(bv_log_fn fn, void* user) { bv::log::setSink(fn, user); }

bv_result BV_CALL bv_set_runtime_paths(const char* bundled_dir, const char* user_dir, const char* manifest) {
    return guarded([&] {
        using bv::runtime::fromUtf8;
        bv::runtime::setGlobalPaths({bundled_dir ? fromUtf8(bundled_dir) : std::filesystem::path{},
                                     user_dir ? fromUtf8(user_dir) : std::filesystem::path{},
                                     manifest ? fromUtf8(manifest) : std::filesystem::path{}});
        return BV_OK;
    });
}

bv_result BV_CALL bv_runtime_import(const char* source_path, bv_feature feature, bv_runtime_status* out) {
    if (!source_path || feature < 0 || feature >= BV_FEATURE_COUNT) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv::runtime::Catalog catalog(bv::runtime::globalPaths());
        bv::runtime::Located located;
        using E = bv::runtime::Catalog::ImportError;
        switch (catalog.importFile(bv::runtime::fromUtf8(source_path), feature, &located)) {
        case E::None: break;
        case E::NoUserDir: return BV_ERR_NOT_INITIALIZED;
        case E::CopyFailed: return BV_ERR_INTERNAL;
        default: return BV_ERR_INVALID_ARGUMENT;
        }
        if (out) bv::fillStatus(*out, feature, &located, BV_RUNTIME_FOUND);
        return BV_OK;
    });
}

bv_result BV_CALL bv_runtime_scan(const char* folder, bv_runtime_candidate* out, uint32_t capacity, uint32_t* count) {
    if (!folder || !count) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv::runtime::Catalog catalog(bv::runtime::globalPaths());
        const auto found = catalog.scan(bv::runtime::fromUtf8(folder));
        *count = static_cast<uint32_t>(found.size());
        for (uint32_t i = 0; out && i < capacity && i < found.size(); ++i) {
            auto& c = out[i];
            c = bv_runtime_candidate{};
            c.size = sizeof(c);
            c.feature = found[i].feature;
            c.origin = found[i].id.origin;
            const auto label = found[i].id.label;
            const auto path = bv::runtime::toUtf8(found[i].path);
            std::memcpy(c.label, label.data(), std::min(label.size(), sizeof(c.label) - 1));
            std::memcpy(c.path, path.data(), std::min(path.size(), sizeof(c.path) - 1));
        }
        return BV_OK;
    });
}

bv_result BV_CALL bv_engine_runtime_status(bv_engine* engine, bv_feature feature, bv_runtime_status* out) {
    if (!engine || !out || feature < 0 || feature >= BV_FEATURE_COUNT) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        *out = engine->engine.runtimeStatus(feature);
        return BV_OK;
    });
}

bv_result BV_CALL bv_engine_stage_stats(bv_engine* engine, const char* stage, bv_stage_stats* out) {
    if (!engine || !stage || !out) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv_stage_stats result{};
        result.size = sizeof(result);
        if (const auto* stats = engine->engine.telemetry().find(stage)) {
            const auto s = stats->snapshot();
            result.samples = s.samples;
            result.avg_us = s.avg;
            result.p50_us = s.p50;
            result.p95_us = s.p95;
            result.p99_us = s.p99;
            result.max_us = s.max;
        }
        *out = result;
        return BV_OK;
    });
}

} // extern "C"
