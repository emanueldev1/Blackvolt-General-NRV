#pragma once

#include "NrArchitecturePolicy.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace bv::ngx {

// DLSS NR (feature 18) through nvngx_dlssnr.dll loaded directly, the way Veyra drives it.
//
// The NR DLL is a self-contained NGX "snippet": it exports its own Init_Ext / CreateFeature /
// EvaluateFeature / ReleaseFeature / Shutdown1. It is initialised with the signed snippet
// application id and expects its caller to be the NGX core, which it checks by asking
// GetModuleFileNameW for the calling module. A shim on the snippet's own import slot answers
// "nvngx.dll" for Blackvolt's module and forwards everything else untouched; no global hook.
//
// On RTX 30 with builds that need it, NVAPI's architecture query is answered as Blackwell for
// the selected adapter only, again through the snippet's own GetProcAddress import.
//
// One session per process (the import shim has a single owner); presenters share it.
class DlssNrSession {
public:
    // Returns the live session for (device, folder) or creates one. `folder` holds
    // nvngx_dlssnr.dll. On failure returns nullptr and a human readable `error`.
    static std::shared_ptr<DlssNrSession> acquire(ID3D12Device* device, const std::filesystem::path& folder,
                                                  NrVariant variant, std::string* error);
    ~DlssNrSession();

    DlssNrSession(const DlssNrSession&) = delete;
    DlssNrSession& operator=(const DlssNrSession&) = delete;

    // Thin, SEH-guarded calls into the snippet. result = NVSDK_NGX_Result, seh = exception code.
    bool createFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle,
                       uint64_t& result, uint32_t& seh);
    bool evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
                  uint64_t& result, uint32_t& seh);
    void releaseFeature(NVSDK_NGX_Handle* handle);

    // Scaling-ratio callback NR asks for at creation: same size, 1.0.
    static void* scalingRatioCallback();

    [[nodiscard]] NrVariant variant() const { return variant_; }
    [[nodiscard]] const std::string& version() const { return version_; } // "310.9.16.0"
    [[nodiscard]] bool ampereRoute() const { return ampereRoute_; }
    [[nodiscard]] ID3D12Device* device() const { return device_; }

    struct Impl;

private:
    DlssNrSession() = default;

    std::unique_ptr<Impl> impl_;
    ID3D12Device* device_ = nullptr;
    std::filesystem::path folder_;
    NrVariant variant_ = NrVariant::Compat;
    std::string version_;
    bool ampereRoute_ = false;
};

} // namespace bv::ngx
