#pragma once

// NGX feature DLLs are closed source and occasionally fault on unusual drivers. These
// wrappers turn a structured exception into an NGX failure so playback keeps running
// with the feature disabled instead of taking the whole app down.
#if defined(BV_HAS_NGX)

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <cstdint>

namespace bv::ngx::guard {

#if defined(_MSC_VER)
inline NVSDK_NGX_Result create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* params,
                               NVSDK_NGX_Handle** handle, uint32_t& seh) {
    seh = 0;
    __try {
        return NVSDK_NGX_D3D12_CreateFeature(list, feature, params, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

inline NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params,
                                 uint32_t& seh) {
    seh = 0;
    __try {
        return NVSDK_NGX_D3D12_EvaluateFeature(list, handle, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

inline void release(NVSDK_NGX_Handle* handle) {
    __try {
        NVSDK_NGX_D3D12_ReleaseFeature(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#else
inline NVSDK_NGX_Result create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* params,
                               NVSDK_NGX_Handle** handle, uint32_t& seh) {
    seh = 0;
    return NVSDK_NGX_D3D12_CreateFeature(list, feature, params, handle);
}
inline NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params,
                                 uint32_t& seh) {
    seh = 0;
    return NVSDK_NGX_D3D12_EvaluateFeature(list, handle, params);
}
inline void release(NVSDK_NGX_Handle* handle) { NVSDK_NGX_D3D12_ReleaseFeature(handle); }
#endif

} // namespace bv::ngx::guard

#endif
