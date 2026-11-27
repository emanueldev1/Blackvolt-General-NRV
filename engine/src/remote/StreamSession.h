#pragma once

#include "ConsoleProfile.h"
#include "player/Sinks.h"
#include "telemetry/Telemetry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct ID3D12Device;

namespace bv::remote {

struct StreamSettings {
    uint32_t width = 1920, height = 1080; // 640x360 .. 1920x1080 (what Remote Play offers)
    uint32_t fps = 60;                     // 30 or 60
    uint32_t bitrateKbps = 0;              // 0 = console default for the resolution
    uint32_t codec = 1;                    // 0 H.264, 1 HEVC, 2 HEVC HDR (PS5)
    bool autoReconnect = true;
    bool wakeIfAsleep = true;
    bool enableDualSense = true;           // haptics/adaptive triggers when the pad supports them
};

enum class StreamState { Idle, Waking, Connecting, Streaming, Reconnecting, Ended, Error };

struct StreamSnapshot {
    StreamState state = StreamState::Idle;
    std::string error;        // last fatal error / quit reason
    std::string codec;
    uint32_t width = 0, height = 0;
    double fps = 0.0;          // frames presented per second (measured)
    double bitrateMbps = 0.0;  // received video bitrate (measured, last second)
    double rttMs = 0.0;        // network round trip measured at connect
    double decodeMs = 0.0;     // average decode+present time, last second
    uint64_t framesPresented = 0;
    uint64_t framesLost = 0;   // reported missing by the protocol layer
    uint64_t framesRecovered = 0; // repaired by FEC
    uint32_t reconnects = 0;
    bool hardwareDecode = false;
    bool gamepadConnected = false;
    std::string gamepadName;
};

// Gamepad state in DualSense terms (what the console expects).
struct GamepadState {
    uint32_t buttons = 0;       // ChiakiControllerButton bit layout (cross = bit 0 ... PS = bit 15)
    uint8_t l2 = 0, r2 = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    struct Touch { int8_t id = -1; uint16_t x = 0, y = 0; } touches[2];
    float gyro[3] = {0, 0, 0};
    float accel[3] = {0, 1, 0};
    float orient[4] = {0, 0, 0, 1};
};

// Where controller input comes from and where feedback goes (SDL3 pads, UI keyboard...).
class IInputDevice {
public:
    virtual ~IInputDevice() = default;
    virtual bool poll(GamepadState& state) = 0; // false = no device right now
    virtual void rumble(uint8_t low, uint8_t high) { (void)low; (void)high; }
    virtual void setLed(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
    virtual void setTriggerEffects(uint8_t typeLeft, const uint8_t* left10, uint8_t typeRight, const uint8_t* right10) {
        (void)typeLeft; (void)left10; (void)typeRight; (void)right10;
    }
    [[nodiscard]] virtual std::string name() const { return {}; }
};

// A live Remote Play session: protocol (chiaki-ng) -> low-latency GPU decode -> immediate
// present, Opus audio -> WASAPI, gamepad -> console, with automatic wake and reconnect.
class StreamSession {
public:
    using AudioSinkFactory = std::function<std::unique_ptr<player::IAudioSink>()>;

    StreamSession(ID3D12Device* device, player::IVideoPresenter& presenter, AudioSinkFactory audio,
                  std::unique_ptr<IInputDevice> input, telemetry::Telemetry& telemetry);
    ~StreamSession();
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    void start(const ConsoleProfile& console, const StreamSettings& settings);
    void stop();

    // Extra input merged with the gamepad (keyboard/mouse mapping from the UI).
    void setOverlayInput(const GamepadState& state);
    void setVolume(float volume);
    void requestKeyframe();
    void sendPsButton(); // tap PS (home)
    void putConsoleToSleep();

    [[nodiscard]] StreamSnapshot snapshot() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace bv::remote
