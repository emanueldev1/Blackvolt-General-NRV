#include "WasapiSink.h"

#if defined(_WIN32)

#include "core/Log.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace bv::audio {
namespace {

using Microsoft::WRL::ComPtr;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class WasapiSink final : public player::IAudioSink {
public:
    bool init() {
        comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_)) ||
            FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_))) {
            log::warn("wasapi: no default output device");
            return false;
        }
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client_->GetMixFormat(&mix))) return false;
        rate_ = static_cast<int>(mix->nSamplesPerSec);
        channels_ = std::clamp<int>(mix->nChannels, 1, 8);
        CoTaskMemFree(mix);

        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = static_cast<WORD>(channels_);
        fmt.Format.nSamplesPerSec = static_cast<DWORD>(rate_);
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = static_cast<WORD>(channels_ * 4);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = channels_ == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        // 100 ms buffer: enough headroom against scheduling hiccups, low enough for snappy seeks.
        const REFERENCE_TIME buffer = 1'000'000;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       buffer, 0, &fmt.Format, nullptr)) ||
            FAILED(client_->GetBufferSize(&bufferFrames_)) || FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
            log::warn("wasapi: initialize failed");
            return false;
        }
        REFERENCE_TIME latency = 0;
        client_->GetStreamLatency(&latency);
        latency_ = static_cast<double>(latency) / 1e7;
        client_->Start();
        log::info("wasapi: {} Hz, {} ch, buffer {} frames", rate_, channels_, bufferFrames_);
        return true;
    }

    ~WasapiSink() override {
        if (client_) client_->Stop();
        render_.Reset();
        client_.Reset();
        device_.Reset();
        if (comInit_) CoUninitialize();
    }

    int sampleRate() const override { return rate_; }
    int channels() const override { return channels_; }

    bool write(const float* samples, std::size_t frames, double pts) override {
        std::size_t done = 0;
        while (done < frames) {
            if (stopped_) return false;
            UINT32 padding = 0;
            {
                std::lock_guard lock(mutex_);
                if (FAILED(client_->GetCurrentPadding(&padding))) return false;
                const UINT32 room = bufferFrames_ - padding;
                const UINT32 n = static_cast<UINT32>(std::min<std::size_t>(room, frames - done));
                if (n > 0) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(render_->GetBuffer(n, &dst))) {
                        std::memcpy(dst, samples + done * channels_, n * channels_ * sizeof(float));
                        render_->ReleaseBuffer(n, 0);
                        done += n;
                        if (!std::isnan(pts)) endPts_ = pts + static_cast<double>(done) / rate_;
                    }
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return true;
    }

    double playedPosition() const override {
        std::lock_guard lock(mutex_);
        if (std::isnan(endPts_)) return kNaN;
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return kNaN;
        // Everything still queued in the device has not been heard yet.
        return endPts_ - static_cast<double>(padding) / rate_ - latency_;
    }

    void setPaused(bool paused) override {
        std::lock_guard lock(mutex_);
        paused_ = paused;
        if (paused) client_->Stop();
        else client_->Start();
    }

    void flush() override {
        std::lock_guard lock(mutex_);
        client_->Stop();
        client_->Reset();
        if (!paused_) client_->Start();
        endPts_ = kNaN;
    }

    void setVolume(float) override {} // volume is applied to samples by the player
    void stop() override { stopped_ = true; }

private:
    bool comInit_ = false;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    UINT32 bufferFrames_ = 0;
    int rate_ = 48000;
    int channels_ = 2;
    double latency_ = 0.0;
    double endPts_ = kNaN;
    bool paused_ = false;
    mutable std::mutex mutex_;
    std::atomic<bool> stopped_{false};
};

} // namespace

std::unique_ptr<player::IAudioSink> createWasapiSink() {
    auto sink = std::make_unique<WasapiSink>();
    if (!sink->init()) return nullptr;
    return sink;
}

} // namespace bv::audio

#else

namespace bv::audio {
std::unique_ptr<player::IAudioSink> createWasapiSink() { return nullptr; }
} // namespace bv::audio

#endif
