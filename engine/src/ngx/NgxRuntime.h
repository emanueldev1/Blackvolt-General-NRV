#pragma once

#include "blackvolt/bv_api.h"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

struct ID3D12Device;
struct IDXGIAdapter;

namespace bv::ngx {

// Owns NGX initialisation on one D3D12 device and answers "can this GPU run feature X
// with the DLLs we have". Feature instances (evaluate calls) are created by pipeline stages.
class NgxRuntime {
public:
    // searchPaths: folders holding nvngx_*.dll, highest priority first.
    // dataPath: writable folder for NGX logs and caches.
    static std::unique_ptr<NgxRuntime> create(ID3D12Device* device, IDXGIAdapter* adapter,
                                              const std::vector<std::filesystem::path>& searchPaths,
                                              const std::filesystem::path& dataPath);
    ~NgxRuntime();

    [[nodiscard]] bool initialized() const { return initialized_; }
    // Support state for a feature whose DLL is present. Cached after the first query.
    bv_runtime_state support(bv_feature feature);

    static constexpr bool kCompiledIn =
#if defined(BV_HAS_NGX)
        true;
#else
        false;
#endif

private:
    NgxRuntime() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;
    std::array<int, BV_FEATURE_COUNT> cached_{};
};

} // namespace bv::ngx
