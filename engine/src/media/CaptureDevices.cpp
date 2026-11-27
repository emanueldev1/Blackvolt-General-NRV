#include "CaptureDevices.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <windows.h>

namespace bv::media {
namespace {

std::string narrow(const wchar_t* w, UINT32 length) {
    if (!w || !length) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(length), out.data(), n, nullptr, nullptr);
    return out;
}

void collect(const GUID& type, const GUID& linkKey, bool audio, std::vector<CaptureDevice>& out) {
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, type);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            CaptureDevice d;
            d.audio = audio;
            WCHAR* text = nullptr;
            UINT32 length = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &text, &length))) {
                d.name = narrow(text, length);
                CoTaskMemFree(text);
            }
            if (SUCCEEDED(devices[i]->GetAllocatedString(linkKey, &text, &length))) {
                d.id = narrow(text, length);
                CoTaskMemFree(text);
            }
            if (!d.name.empty()) out.push_back(std::move(d));
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attrs->Release();
}

} // namespace

std::vector<CaptureDevice> enumerateCaptureDevices() {
    std::vector<CaptureDevice> out;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        collect(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                false, out);
        collect(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
                true, out);
        MFShutdown();
    } else {
        log::warn("capture: Media Foundation unavailable");
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

} // namespace bv::media

#else

#include <filesystem>

namespace bv::media {

std::vector<CaptureDevice> enumerateCaptureDevices() {
    std::vector<CaptureDevice> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/dev", ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("video", 0) == 0) out.push_back({e.path().string(), e.path().string(), false});
    }
    return out;
}

} // namespace bv::media

#endif
