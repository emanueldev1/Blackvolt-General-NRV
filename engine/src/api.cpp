#include "blackvolt/bv_api.h"

#include "core/Engine.h"
#include "core/EngineHandle.h"
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
#include <vector>

#if defined(_WIN32)
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif



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
            c.nr_variant = static_cast<uint32_t>(found[i].nrVariant);
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

namespace {
#define BV_COPY(dst, src) copyText(dst, sizeof(dst), src)
void copyText(char* dst, std::size_t size, const std::string& src) {
    const auto n = std::min(src.size(), size - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void fillNrInfo(bv::runtime::Catalog& catalog, uint32_t variant, bv_nr_runtime_info& out) {
    out = bv_nr_runtime_info{};
    out.size = sizeof(out);
    out.variant = variant;
    const auto located = catalog.locateNr(static_cast<bv::ngx::NrVariant>(variant));
    if (!located) return;
    out.installed = 1;
    out.origin = located->id.origin;
    std::error_code ec;
    out.bytes = std::filesystem::file_size(located->path, ec);
    BV_COPY(out.version, bv::runtime::Catalog::fileVersion(located->path));
    BV_COPY(out.label, located->id.label);
    if (located->id.known) BV_COPY(out.sha256, located->id.known->sha256);
    BV_COPY(out.path, bv::runtime::toUtf8(located->path));
}
} // namespace

bv_result BV_CALL bv_nr_runtime_get(uint32_t variant, bv_nr_runtime_info* out) {
    if (!out || variant < BV_NR_COMPAT || variant > BV_NR_ORIGINAL) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv::runtime::Catalog catalog(bv::runtime::globalPaths());
        fillNrInfo(catalog, variant, *out);
        return BV_OK;
    });
}

bv_result BV_CALL bv_nr_runtime_import(const char* source_path, uint32_t variant, bv_nr_runtime_info* out) {
    if (!source_path || variant < BV_NR_COMPAT || variant > BV_NR_ORIGINAL) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv::runtime::Catalog catalog(bv::runtime::globalPaths());
        using E = bv::runtime::Catalog::ImportError;
        switch (catalog.importNr(bv::runtime::fromUtf8(source_path), static_cast<bv::ngx::NrVariant>(variant), nullptr)) {
        case E::None: break;
        case E::NoUserDir: return BV_ERR_NOT_INITIALIZED;
        case E::CopyFailed: return BV_ERR_INTERNAL;
        default: return BV_ERR_INVALID_ARGUMENT;
        }
        if (out) fillNrInfo(catalog, variant, *out);
        return BV_OK;
    });
}

bv_result BV_CALL bv_nr_runtime_remove(uint32_t variant) {
    if (variant < BV_NR_COMPAT || variant > BV_NR_ORIGINAL) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv::runtime::Catalog catalog(bv::runtime::globalPaths());
        return catalog.removeNr(static_cast<bv::ngx::NrVariant>(variant)) ? BV_OK : BV_ERR_INTERNAL;
    });
}

bv_result BV_CALL bv_engine_stage_history(bv_engine* engine, const char* stage, float* out, uint32_t capacity,
                                          uint32_t* count) {
    if (!engine || !stage || !count) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        *count = 0;
        if (const auto* stats = engine->engine.telemetry().find(stage)) {
            *count = static_cast<uint32_t>(stats->recent(out, capacity));
        }
        return BV_OK;
    });
}

bv_result BV_CALL bv_engine_gpu_memory(bv_engine* engine, bv_gpu_memory* out) {
    if (!engine || !out) return BV_ERR_INVALID_ARGUMENT;
    return guarded([&] {
        bv_gpu_memory m{};
        m.size = sizeof(m);
        m.dedicated_bytes = engine->engine.gpu().adapter().dedicated_vram_bytes;
#if defined(_WIN32)
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (auto* adapter = engine->engine.gpu().dxgiAdapter();
            adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                m.usage_bytes = info.CurrentUsage;
                m.budget_bytes = info.Budget;
            }
        }
#endif
        *out = m;
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
