// Adapted from Veyra 2.2.1 src/ngx/DlssNrRuntimeAdapter.cpp (GPL-3.0); see docs/THIRD_PARTY_PORTS.md.
#include "DlssNrSession.h"

#include "core/Log.h"

#include <mutex>

#if defined(_WIN32) && defined(BV_HAS_NGX)

#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <atomic>
#include <cstring>
#include <format>

namespace bv::ngx {

namespace {

// Signed snippet application id from the NR call contract. Not Blackvolt's NGX project id.
constexpr unsigned long long kSnippetAppId = 0x0876232Cull;
constexpr NVSDK_NGX_Feature kFeatureNr = static_cast<NVSDK_NGX_Feature>(18);

using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                 const NVSDK_NGX_Parameter*);
using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                         const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

// ---------------------------------------------------------------------------------------------
// System NVAPI, loaded once and pinned. A runtime's own FreeLibrary must never be the one that
// detaches the driver module (that deadlocked teardown in the field).
HMODULE processNvapi() {
    static const HMODULE module = []() -> HMODULE {
        wchar_t dir[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (!n || n >= MAX_PATH) return nullptr;
        const std::wstring path = std::wstring(dir) + L"\\nvapi64.dll";
        HMODULE loaded = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loaded) {
            log::warn("nr: system NVAPI not loaded ({})", GetLastError());
            return nullptr;
        }
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(loaded), &pinned);
        return loaded;
    }();
    return module;
}

// ---------------------------------------------------------------------------------------------
// Caller-name shim on the snippet's GetModuleFileNameW import.
using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
GetModuleFileNameWFn g_realGetModuleFileNameW = nullptr;
HMODULE g_callerModule = nullptr;

constexpr wchar_t kShimModuleName[] = L"nvngx.dll";
constexpr DWORD kShimModuleNameChars = 9;

__declspec(noinline) DWORD WINAPI SnippetGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    if (g_callerModule == nullptr) {
        // Blackvolt's module, from this function's own address (never guessed by file name).
        HMODULE caller = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&SnippetGetModuleFileNameW), &caller)) {
            g_callerModule = caller;
        }
    }
    if (module != nullptr && module == g_callerModule) {
        if (size == 0) return 0;
        if (size <= kShimModuleNameChars) { // same semantics as the real API when truncating
            for (DWORD i = 0; i + 1 < size; ++i) filename[i] = kShimModuleName[i];
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        for (DWORD i = 0; i <= kShimModuleNameChars; ++i) filename[i] = kShimModuleName[i];
        return kShimModuleNameChars;
    }
    if (!g_realGetModuleFileNameW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return 0;
    }
    return g_realGetModuleFileNameW(module, filename, size);
}

// Address of the IAT slot through which `module` imports `functionName` from kernel32 or an
// API set. nullptr when there is no such import.
void** findImportSlot(HMODULE module, const char* functionName) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name != 0; ++d) {
        const char* dll = reinterpret_cast<const char*>(base + d->Name);
        const bool kernel32 = _stricmp(dll, "kernel32.dll") == 0;
        const bool apiSet = _strnicmp(dll, "api-ms-", 7) == 0 || _strnicmp(dll, "ext-ms-", 7) == 0;
        if (!kernel32 && !apiSet) continue;
        // Without OriginalFirstThunk the names are gone once the image is bound.
        if (d->OriginalFirstThunk == 0 || d->FirstThunk == 0) continue;
        const auto* lookup = reinterpret_cast<const ULONGLONG*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (size_t i = 0; lookup[i] != 0; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup[i])) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + static_cast<size_t>(lookup[i]));
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), functionName) == 0)
                return reinterpret_cast<void**>(&iat[i].u1.Function);
        }
    }
    return nullptr;
}

bool exchangeImport(void** slot, void* replacement, void*& previous) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    previous = InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    if (VirtualProtect(slot, sizeof(void*), old, &ignored)) {
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        return true;
    }
    InterlockedExchangePointer(slot, previous); // still writable: roll back before failing
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return false;
}

// ---------------------------------------------------------------------------------------------
// Ampere route: answer NVAPI's architecture query as Blackwell for the selected adapter, only
// for lookups made by the NR module. Implemented against NVAPI's public query ABI.
using QueryNvApi = void*(__cdecl*)(unsigned);
struct NvArchInfo { unsigned version, architecture, implementation, revision; };
using GetNvArch = int(__cdecl*)(void*, NvArchInfo*);
struct LogicalGpuData {
    unsigned version;
    void* osAdapterId;
    unsigned physicalGpuCount;
    void* physicalGpuHandles[64];
    unsigned reserved[8];
};
struct AmpereContext {
    HMODULE nvapi = nullptr;
    QueryNvApi query = nullptr;
    GetNvArch getArch = nullptr;
    decltype(&GetProcAddress) getProc = nullptr;
    void* selected[64]{};
    unsigned count = 0;
    std::atomic_uint64_t rewrites{0};
};
AmpereContext g_ampere;
constexpr unsigned kArchQuery = 0xD8265D24u;

int __cdecl ampereGetArch(void* gpu, NvArchInfo* info) {
    const int result = g_ampere.getArch(gpu, info);
    if (result != 0 || !info) return result;
    bool selected = false;
    for (unsigned i = 0; i < g_ampere.count; ++i) selected |= gpu == g_ampere.selected[i];
    if (rewriteNrAmpereArchitecture(info->architecture, selected, result)) g_ampere.rewrites.fetch_add(1);
    return result;
}

void* __cdecl ampereQuery(unsigned id) {
    if (id == kArchQuery) return reinterpret_cast<void*>(&ampereGetArch);
    return g_ampere.query(id);
}

FARPROC WINAPI ampereGetProc(HMODULE module, LPCSTR name) {
    if (module == g_ampere.nvapi && reinterpret_cast<uintptr_t>(name) > 0xFFFF &&
        std::strcmp(name, "nvapi_QueryInterface") == 0) {
        return reinterpret_cast<FARPROC>(&ampereQuery);
    }
    return g_ampere.getProc(module, name);
}

void clearAmpere() {
    g_ampere.nvapi = nullptr;
    g_ampere.query = nullptr;
    g_ampere.getArch = nullptr;
    g_ampere.getProc = nullptr;
    g_ampere.count = 0;
}

// ---------------------------------------------------------------------------------------------
// SEH guards: the snippet is closed source; a fault disables NR instead of closing the app.
__declspec(noinline) NVSDK_NGX_Result callInit(InitExtFn fn, const wchar_t* dir, ID3D12Device* device, uint32_t& seh) {
    seh = 0;
    __try {
        return fn(kSnippetAppId, dir, device, NVSDK_NGX_Version_API, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

__declspec(noinline) NVSDK_NGX_Result callCreate(CreateFeatureFn fn, ID3D12GraphicsCommandList* list,
                                                 NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle, uint32_t& seh) {
    seh = 0;
    __try {
        return fn(list, kFeatureNr, params, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

__declspec(noinline) NVSDK_NGX_Result callEvaluate(EvaluateFeatureFn fn, ID3D12GraphicsCommandList* list,
                                                   const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
                                                   uint32_t& seh) {
    seh = 0;
    __try {
        return fn(list, handle, params, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

__declspec(noinline) NVSDK_NGX_Result callRelease(ReleaseFeatureFn fn, NVSDK_NGX_Handle* handle, uint32_t& seh) {
    seh = 0;
    __try {
        return fn(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

__declspec(noinline) NVSDK_NGX_Result callShutdown(ShutdownFn fn, ID3D12Device* device, uint32_t& seh) {
    seh = 0;
    __try {
        return fn(device);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = GetExceptionCode();
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

NVSDK_NGX_Result NVSDK_CONV scalingRatio(NVSDK_NGX_Parameter* params) {
    if (params) params->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
}

std::mutex g_sessionMutex;
std::weak_ptr<DlssNrSession> g_session;

} // namespace

struct DlssNrSession::Impl {
    HMODULE module = nullptr;
    InitExtFn initExt = nullptr;
    CreateFeatureFn createFeature = nullptr;
    EvaluateFeatureFn evaluateFeature = nullptr;
    ReleaseFeatureFn releaseFeature = nullptr;
    ShutdownFn shutdown = nullptr;
    void** shimSlot = nullptr;
    void* shimOriginal = nullptr;
    void** archSlot = nullptr;
    void* archOriginal = nullptr;
    bool initialized = false;
    uint32_t versionMs = 0, versionLs = 0;

    bool load(const std::filesystem::path& folder, std::string& error) {
        const auto dll = folder / L"nvngx_dlssnr.dll";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dll, ec)) {
            error = "Falta nvngx_dlssnr.dll. Descárgalo desde Runtimes NVIDIA.";
            return false;
        }
        processNvapi(); // pin before the snippet can load and free it
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            error = std::format("No se pudo cargar nvngx_dlssnr.dll (error {}).", GetLastError());
            return false;
        }
        initExt = reinterpret_cast<InitExtFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext"));
        createFeature = reinterpret_cast<CreateFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
        evaluateFeature = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        releaseFeature = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
        if (!initExt || !createFeature || !evaluateFeature || !releaseFeature || !shutdown) {
            error = "Ese nvngx_dlssnr.dll no exporta la interfaz esperada.";
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        readVersion();
        return true;
    }

    void readVersion() {
        // VS_FIXEDFILEINFO straight from the loaded image (RT_VERSION = 16), no version.lib.
        HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
        if (!info) return;
        const auto size = SizeofResource(module, info);
        const auto* bytes = static_cast<const uint8_t*>(LockResource(LoadResource(module, info)));
        for (DWORD at = 0; bytes && at + 52 <= size; at += 4) {
            uint32_t signature = 0;
            std::memcpy(&signature, bytes + at, 4);
            if (signature != 0xFEEF04BDu) continue;
            std::memcpy(&versionMs, bytes + at + 8, 4);
            std::memcpy(&versionLs, bytes + at + 12, 4);
            break;
        }
    }

    bool installShim(std::string& error) {
        void** slot = findImportSlot(module, "GetModuleFileNameW");
        if (!slot || !*slot) {
            error = "nvngx_dlssnr.dll no importa GetModuleFileNameW; build no compatible.";
            return false;
        }
        g_realGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFn>(*slot);
        g_callerModule = nullptr;
        void* previous = nullptr;
        if (!exchangeImport(slot, reinterpret_cast<void*>(&SnippetGetModuleFileNameW), previous)) {
            g_realGetModuleFileNameW = nullptr;
            error = "No se pudo preparar la compatibilidad del llamador.";
            return false;
        }
        shimSlot = slot;
        shimOriginal = previous;
        return true;
    }

    // Returns false only on a hard failure; "not needed" is success.
    bool installAmpere(ID3D12Device* device, bool& installed, std::string& error) {
        installed = false;
        if (versionMs && !nrRuntimeNeedsAmpereRewrite(versionMs, versionLs)) return true;
        g_ampere.nvapi = processNvapi();
        if (!g_ampere.nvapi) { error = "NVAPI del sistema no disponible."; clearAmpere(); return false; }
        g_ampere.query = reinterpret_cast<QueryNvApi>(GetProcAddress(g_ampere.nvapi, "nvapi_QueryInterface"));
        if (!g_ampere.query) { error = "NVAPI sin nvapi_QueryInterface."; clearAmpere(); return false; }
        const auto init = reinterpret_cast<int(__cdecl*)()>(g_ampere.query(0x0150E828u));
        const auto enumerate = reinterpret_cast<int(__cdecl*)(void**, unsigned*)>(g_ampere.query(0x48B3EA59u));
        const auto getLogical = reinterpret_cast<int(__cdecl*)(void*, LogicalGpuData*)>(g_ampere.query(0x842B066Eu));
        g_ampere.getArch = reinterpret_cast<GetNvArch>(g_ampere.query(kArchQuery));
        if (!init || !enumerate || !getLogical || !g_ampere.getArch || init() != 0) {
            error = "NVAPI no respondió a las consultas de arquitectura.";
            clearAmpere();
            return false;
        }
        void* logical[64]{};
        unsigned count = 0;
        if (enumerate(logical, &count) != 0 || count > 64) {
            error = "NVAPI no pudo enumerar las GPUs.";
            clearAmpere();
            return false;
        }
        const LUID target = device->GetAdapterLuid();
        bool ampere = false;
        for (unsigned i = 0; i < count && !g_ampere.count; ++i) {
            LUID luid{};
            LogicalGpuData data{};
            data.version = sizeof(data) | (1u << 16);
            data.osAdapterId = &luid;
            if (getLogical(logical[i], &data) != 0) continue;
            if (luid.LowPart != target.LowPart || luid.HighPart != target.HighPart) continue;
            if (data.physicalGpuCount == 0 || data.physicalGpuCount > 64) break;
            g_ampere.count = data.physicalGpuCount;
            for (unsigned j = 0; j < data.physicalGpuCount; ++j) {
                g_ampere.selected[j] = data.physicalGpuHandles[j];
                NvArchInfo arch{sizeof(NvArchInfo) | (1u << 16), 0, 0, 0};
                if (g_ampere.getArch(g_ampere.selected[j], &arch) != 0) {
                    error = "No se pudo leer la arquitectura de la GPU.";
                    clearAmpere();
                    return false;
                }
                const unsigned family = arch.architecture & 0xFFFFFFF0u;
                log::info("nr: adapter architecture 0x{:X}", arch.architecture);
                if (family == 0x170u) ampere = true;
                else if (family != 0x190u && family < 0x1A0u) {
                    error = "Este build de NR necesita una RTX 30 o posterior (usa el build compatible SF para RTX 20).";
                    clearAmpere();
                    return false;
                }
            }
        }
        if (!g_ampere.count) { error = "NVAPI no reconoce la GPU seleccionada."; clearAmpere(); return false; }
        if (!ampere) { clearAmpere(); return true; } // Ada/Blackwell: nothing to rewrite
        void** slot = findImportSlot(module, "GetProcAddress");
        if (!slot || !*slot) { error = "nvngx_dlssnr.dll no importa GetProcAddress."; clearAmpere(); return false; }
        g_ampere.getProc = reinterpret_cast<decltype(&GetProcAddress)>(*slot);
        g_ampere.rewrites = 0;
        if (!exchangeImport(slot, reinterpret_cast<void*>(&ampereGetProc), archOriginal)) {
            error = "No se pudo activar la ruta RTX 30.";
            clearAmpere();
            return false;
        }
        archSlot = slot;
        installed = true;
        log::info("nr: RTX 30 route installed on the NR module only");
        return true;
    }

    void unload(ID3D12Device* device) {
        if (initialized && shutdown) {
            uint32_t seh = 0;
            const auto r = callShutdown(shutdown, device, seh);
            if (NVSDK_NGX_FAILED(r) || seh) log::warn("nr: Shutdown1 0x{:08X} seh 0x{:08X}", static_cast<uint32_t>(r), seh);
            initialized = false;
        }
        void* previous = nullptr;
        if (archSlot) {
            exchangeImport(archSlot, archOriginal, previous);
            log::info("nr: RTX 30 route removed ({} rewrites)", g_ampere.rewrites.load());
            archSlot = nullptr;
            clearAmpere();
        }
        if (shimSlot) {
            exchangeImport(shimSlot, shimOriginal, previous);
            shimSlot = nullptr;
            g_realGetModuleFileNameW = nullptr;
            g_callerModule = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
    }
};

std::shared_ptr<DlssNrSession> DlssNrSession::acquire(ID3D12Device* device, const std::filesystem::path& folder,
                                                      NrVariant variant, std::string* error) {
    std::string err;
    std::lock_guard lock(g_sessionMutex);
    if (auto live = g_session.lock()) {
        if (live->device_ == device && live->folder_ == folder) return live;
        if (error) *error = "DLSS NR ya está en uso en otra ventana con otro runtime; ciérrala para cambiarlo.";
        return nullptr;
    }
    std::shared_ptr<DlssNrSession> s(new DlssNrSession());
    s->impl_ = std::make_unique<Impl>();
    s->device_ = device;
    s->folder_ = folder;
    s->variant_ = variant;
    auto& im = *s->impl_;
    bool ampere = false;
    if (!im.load(folder, err) || !im.installShim(err) ||
        (variant == NrVariant::Compat && !im.installAmpere(device, ampere, err))) {
        im.unload(device);
        if (error) *error = err;
        log::warn("nr: {}", err);
        return nullptr;
    }
    s->ampereRoute_ = ampere;
    if (im.versionMs) {
        s->version_ = std::format("{}.{}.{}.{}", im.versionMs >> 16, im.versionMs & 0xFFFFu, im.versionLs >> 16,
                                  im.versionLs & 0xFFFFu);
    }
    uint32_t seh = 0;
    const auto r = callInit(im.initExt, folder.c_str(), device, seh);
    if (NVSDK_NGX_FAILED(r) || seh) {
        log::warn("nr: Init_Ext 0x{:08X} seh 0x{:08X}", static_cast<uint32_t>(r), seh);
        im.unload(device);
        if (error) {
            *error = seh ? std::format("nvngx_dlssnr.dll falló al iniciar (excepción 0x{:08X}).", seh)
                         : std::format("NVIDIA rechazó DLSS NR (0x{:08X}). Prueba el otro runtime o actualiza el driver.",
                                       static_cast<uint32_t>(r));
        }
        return nullptr;
    }
    im.initialized = true;
    log::info("nr: {} {} ready{}", nrVariantName(variant), s->version_, ampere ? " (RTX 30 route)" : "");
    g_session = s;
    return s;
}

DlssNrSession::~DlssNrSession() {
    if (impl_) impl_->unload(device_);
}

bool DlssNrSession::createFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params,
                                  NVSDK_NGX_Handle** handle, uint64_t& result, uint32_t& seh) {
    *handle = nullptr;
    result = static_cast<uint64_t>(callCreate(impl_->createFeature, list, params, handle, seh));
    return seh == 0 && NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result)) && *handle;
}

bool DlssNrSession::evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                             const NVSDK_NGX_Parameter* params, uint64_t& result, uint32_t& seh) {
    result = static_cast<uint64_t>(callEvaluate(impl_->evaluateFeature, list, handle, params, seh));
    return seh == 0 && NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result));
}

void DlssNrSession::releaseFeature(NVSDK_NGX_Handle* handle) {
    if (!handle) return;
    uint32_t seh = 0;
    callRelease(impl_->releaseFeature, handle, seh);
}

void* DlssNrSession::scalingRatioCallback() { return reinterpret_cast<void*>(&scalingRatio); }

} // namespace bv::ngx

#else

namespace bv::ngx {

struct DlssNrSession::Impl {};

std::shared_ptr<DlssNrSession> DlssNrSession::acquire(ID3D12Device*, const std::filesystem::path&, NrVariant,
                                                      std::string* error) {
    if (error) *error = "Este build del motor no incluye NGX (DLSS SDK).";
    return nullptr;
}
DlssNrSession::~DlssNrSession() = default;
bool DlssNrSession::createFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**, uint64_t&,
                                  uint32_t&) {
    return false;
}
bool DlssNrSession::evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                             uint64_t&, uint32_t&) {
    return false;
}
void DlssNrSession::releaseFeature(NVSDK_NGX_Handle*) {}
void* DlssNrSession::scalingRatioCallback() { return nullptr; }

} // namespace bv::ngx

#endif
