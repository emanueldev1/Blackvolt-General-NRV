#include "NgxRuntime.h"

#include "NgxFeatures.h"
#include "core/Log.h"

#if defined(_WIN32) && defined(BV_HAS_NGX)
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <nvsdk_ngx.h>
#  include <nvsdk_ngx_defs.h>
#  include <string>
#endif

namespace bv::ngx {

// Identifies Blackvolt to NGX (custom engine project ID, as the SDK requires for non-registered apps).
static constexpr const char* kProjectId = "6f1b7c2e-4a8d-4f3a-9b5e-b1ac4c7e2d90";
static constexpr const char* kEngineVersion = "0.1";

#if defined(_WIN32) && defined(BV_HAS_NGX)

struct NgxRuntime::Impl {
    ID3D12Device* device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    std::vector<std::wstring> paths;
    std::vector<const wchar_t*> pathPtrs;
    std::wstring dataPath;
    NVSDK_NGX_FeatureCommonInfo common{};
};

std::unique_ptr<NgxRuntime> NgxRuntime::create(ID3D12Device* device, IDXGIAdapter* adapter,
                                               const std::vector<std::filesystem::path>& searchPaths,
                                               const std::filesystem::path& dataPath) {
    std::unique_ptr<NgxRuntime> rt(new NgxRuntime());
    rt->impl_ = std::make_unique<Impl>();
    auto& im = *rt->impl_;
    im.device = device;
    im.adapter = adapter;
    for (const auto& p : searchPaths) im.paths.push_back(p.wstring());
    for (const auto& p : im.paths) im.pathPtrs.push_back(p.c_str());
    im.dataPath = dataPath.wstring();
    std::error_code ec;
    std::filesystem::create_directories(dataPath, ec);

    im.common.PathListInfo.Path = im.pathPtrs.data();
    im.common.PathListInfo.Length = static_cast<unsigned int>(im.pathPtrs.size());
    im.common.LoggingInfo.LoggingCallback = nullptr;
    im.common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    const auto r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                       im.dataPath.c_str(), device, &im.common);
    rt->initialized_ = NVSDK_NGX_SUCCEED(r);
    if (rt->initialized_) {
        log::info("NGX initialised ({} runtime folder(s))", im.paths.size());
    } else {
        log::warn("NGX init failed: 0x{:08X}", static_cast<uint32_t>(r));
    }
    return rt;
}

NgxRuntime::~NgxRuntime() {
    if (initialized_ && impl_ && impl_->device) NVSDK_NGX_D3D12_Shutdown1(impl_->device);
}

bv_runtime_state NgxRuntime::support(bv_feature feature) {
    if (!initialized_) return BV_RUNTIME_NGX_UNAVAILABLE;
    auto& slot = cached_[feature];
    if (slot != 0) return static_cast<bv_runtime_state>(slot - 1);

    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = static_cast<NVSDK_NGX_Feature>(featureId(feature));
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = impl_->dataPath.c_str();
    info.FeatureInfo = &impl_->common;

    NVSDK_NGX_FeatureRequirement req{};
    const auto r = NVSDK_NGX_D3D12_GetFeatureRequirements(impl_->adapter, &info, &req);
    const auto state = NVSDK_NGX_SUCCEED(r) ? stateFromSupportBits(static_cast<uint32_t>(req.FeatureSupported))
                                            : BV_RUNTIME_FOUND;
    slot = static_cast<int>(state) + 1;
    return state;
}

#else

struct NgxRuntime::Impl {};

std::unique_ptr<NgxRuntime> NgxRuntime::create(ID3D12Device*, IDXGIAdapter*, const std::vector<std::filesystem::path>&,
                                               const std::filesystem::path&) {
    return std::unique_ptr<NgxRuntime>(new NgxRuntime());
}

NgxRuntime::~NgxRuntime() = default;

bv_runtime_state NgxRuntime::support(bv_feature) { return BV_RUNTIME_NGX_UNAVAILABLE; }

#endif

} // namespace bv::ngx
