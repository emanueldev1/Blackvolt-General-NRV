#include "SdlGamepad.h"

#ifdef BV_HAS_SDL3

#include "core/Log.h"

#include <chiaki/controller.h>
#include <chiaki/orientation.h>

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace bv::remote {
namespace {

// DualSense output report layout used by SDL_SendGamepadEffect (see SDL's testcontroller).
struct DualSenseEffects {
    uint8_t enableBits1, enableBits2, rumbleRight, rumbleLeft;
    uint8_t headphoneVolume, speakerVolume, micVolume, audioEnableBits, micLightMode, audioMuteBits;
    uint8_t rightTrigger[11];
    uint8_t leftTrigger[11];
    uint8_t unknown1[6];
    uint8_t ledFlags, unknown2[2], ledAnim, ledBrightness, padLights, ledRed, ledGreen, ledBlue;
};
static_assert(sizeof(DualSenseEffects) == 47);

constexpr float kTouchpadW = 1920.0f, kTouchpadH = 942.0f;

class SdlGamepad final : public IInputDevice {
public:
    ~SdlGamepad() override {
        if (pad_) SDL_CloseGamepad(pad_);
        if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }

    bool poll(GamepadState& s) override {
        if (!ensureInit()) return false;
        SDL_PumpEvents();
        if (pad_ && !SDL_GamepadConnected(pad_)) {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
            log::info("gamepad: disconnected");
        }
        if (!pad_) openFirst();
        if (!pad_) return false;
        SDL_UpdateGamepads();
        applyFeedback();

        auto b = [&](SDL_GamepadButton btn, uint32_t bit) {
            if (SDL_GetGamepadButton(pad_, btn)) s.buttons |= bit;
        };
        b(SDL_GAMEPAD_BUTTON_SOUTH, CHIAKI_CONTROLLER_BUTTON_CROSS);
        b(SDL_GAMEPAD_BUTTON_EAST, CHIAKI_CONTROLLER_BUTTON_MOON);
        b(SDL_GAMEPAD_BUTTON_WEST, CHIAKI_CONTROLLER_BUTTON_BOX);
        b(SDL_GAMEPAD_BUTTON_NORTH, CHIAKI_CONTROLLER_BUTTON_PYRAMID);
        b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT);
        b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT);
        b(SDL_GAMEPAD_BUTTON_DPAD_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP);
        b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN);
        b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, CHIAKI_CONTROLLER_BUTTON_L1);
        b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, CHIAKI_CONTROLLER_BUTTON_R1);
        b(SDL_GAMEPAD_BUTTON_LEFT_STICK, CHIAKI_CONTROLLER_BUTTON_L3);
        b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, CHIAKI_CONTROLLER_BUTTON_R3);
        b(SDL_GAMEPAD_BUTTON_START, CHIAKI_CONTROLLER_BUTTON_OPTIONS);
        b(SDL_GAMEPAD_BUTTON_BACK, CHIAKI_CONTROLLER_BUTTON_SHARE);
        b(SDL_GAMEPAD_BUTTON_TOUCHPAD, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD);
        b(SDL_GAMEPAD_BUTTON_GUIDE, CHIAKI_CONTROLLER_BUTTON_PS);

        auto trigger = [&](SDL_GamepadAxis a) {
            const int v = SDL_GetGamepadAxis(pad_, a);
            return static_cast<uint8_t>(v <= 0 ? 0 : (v * 255 + 16383) / 32767);
        };
        s.l2 = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        s.r2 = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        s.leftX = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
        s.leftY = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY);
        s.rightX = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX);
        s.rightY = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY);

        // Touchpad: stable ids per finger, like the real pad.
        if (SDL_GetNumGamepadTouchpads(pad_) > 0) {
            for (int f = 0; f < 2; ++f) {
                bool down = false;
                float x = 0, y = 0, pressure = 0;
                SDL_GetGamepadTouchpadFinger(pad_, 0, f, &down, &x, &y, &pressure);
                if (down) {
                    if (touchIds_[f] < 0) touchIds_[f] = static_cast<int8_t>(nextTouchId_++ & 0x7f);
                    s.touches[f].id = touchIds_[f];
                    s.touches[f].x = static_cast<uint16_t>(x * (kTouchpadW - 1));
                    s.touches[f].y = static_cast<uint16_t>(y * (kTouchpadH - 1));
                } else {
                    touchIds_[f] = -1;
                    s.touches[f].id = -1;
                }
            }
        }

        if (motion_) {
            float g[3] = {0, 0, 0}, a[3] = {0, 0, 0};
            SDL_GetGamepadSensorData(pad_, SDL_SENSOR_GYRO, g, 3);
            SDL_GetGamepadSensorData(pad_, SDL_SENSOR_ACCEL, a, 3);
            for (float& v : a) v /= SDL_STANDARD_GRAVITY;
            const auto us = static_cast<uint32_t>(SDL_GetTicksNS() / 1000);
            ChiakiAccelNewZero zero{};
            chiaki_accel_new_zero_set_inactive(&zero, true);
            chiaki_orientation_tracker_update(&orientation_, g[0], g[1], g[2], a[0], a[1], a[2], &zero, false, us);
            ChiakiControllerState tmp;
            chiaki_controller_state_set_idle(&tmp);
            chiaki_orientation_tracker_apply_to_controller_state(&orientation_, &tmp);
            s.gyro[0] = tmp.gyro_x; s.gyro[1] = tmp.gyro_y; s.gyro[2] = tmp.gyro_z;
            s.accel[0] = tmp.accel_x; s.accel[1] = tmp.accel_y; s.accel[2] = tmp.accel_z;
            s.orient[0] = tmp.orient_x; s.orient[1] = tmp.orient_y; s.orient[2] = tmp.orient_z; s.orient[3] = tmp.orient_w;
        }
        return true;
    }

    // Feedback arrives on protocol threads; applied on the input thread in poll().
    void rumble(uint8_t low, uint8_t high) override {
        std::lock_guard lock(fbMutex_);
        rumble_ = {low, high};
        rumbleDirty_ = true;
    }
    void setLed(uint8_t r, uint8_t g, uint8_t b) override {
        std::lock_guard lock(fbMutex_);
        led_ = {r, g, b};
        ledDirty_ = true;
    }
    void setTriggerEffects(uint8_t typeLeft, const uint8_t* left10, uint8_t typeRight, const uint8_t* right10) override {
        std::lock_guard lock(fbMutex_);
        triggers_.left[0] = typeLeft;
        std::memcpy(triggers_.left + 1, left10, 10);
        triggers_.right[0] = typeRight;
        std::memcpy(triggers_.right + 1, right10, 10);
        triggersDirty_ = true;
    }

    [[nodiscard]] std::string name() const override {
        std::lock_guard lock(nameMutex_);
        return name_;
    }

private:
    bool ensureInit() {
        if (initialized_) return true;
        if (initFailed_) return false;
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1"); // full DualSense/DS4 reports: rumble, gyro, touchpad
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            log::warn("gamepad: SDL init failed: {}", SDL_GetError());
            initFailed_ = true;
            return false;
        }
        initialized_ = true;
        return true;
    }

    void openFirst() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids) return;
        // Prefer a PlayStation pad when several are connected.
        SDL_JoystickID pick = 0;
        for (int i = 0; i < count && !pick; ++i) {
            const auto t = SDL_GetGamepadTypeForID(ids[i]);
            if (t == SDL_GAMEPAD_TYPE_PS5 || t == SDL_GAMEPAD_TYPE_PS4) pick = ids[i];
        }
        if (!pick && count > 0) pick = ids[0];
        SDL_free(ids);
        if (!pick) return;
        pad_ = SDL_OpenGamepad(pick);
        if (!pad_) return;
        motion_ = SDL_GamepadHasSensor(pad_, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad_, SDL_SENSOR_ACCEL) &&
                  SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_GYRO, true) &&
                  SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_ACCEL, true);
        chiaki_orientation_tracker_init(&orientation_);
        dualSense_ = SDL_GetGamepadType(pad_) == SDL_GAMEPAD_TYPE_PS5;
        {
            std::lock_guard lock(nameMutex_);
            const char* n = SDL_GetGamepadName(pad_);
            name_ = n ? n : "Mando";
        }
        log::info("gamepad: {} (motion {}, adaptive triggers {})", name(), motion_ ? "on" : "off",
                  dualSense_ ? "on" : "off");
        std::lock_guard lock(fbMutex_);
        ledDirty_ = rumbleDirty_ = true;
    }

    void applyFeedback() {
        std::lock_guard lock(fbMutex_);
        if (rumbleDirty_) {
            SDL_RumbleGamepad(pad_, static_cast<Uint16>(rumble_[0] * 257), static_cast<Uint16>(rumble_[1] * 257), 5000);
            rumbleDirty_ = false;
        }
        if (ledDirty_) {
            SDL_SetGamepadLED(pad_, led_[0], led_[1], led_[2]);
            ledDirty_ = false;
        }
        if (triggersDirty_ && dualSense_) {
            DualSenseEffects fx{};
            fx.enableBits1 = 0x04 | 0x08; // right + left trigger effects
            std::memcpy(fx.rightTrigger, triggers_.right, 11);
            std::memcpy(fx.leftTrigger, triggers_.left, 11);
            SDL_SendGamepadEffect(pad_, &fx, sizeof(fx));
            triggersDirty_ = false;
        }
    }

    bool initialized_ = false, initFailed_ = false;
    SDL_Gamepad* pad_ = nullptr;
    bool motion_ = false, dualSense_ = false;
    ChiakiOrientationTracker orientation_{};
    std::array<int8_t, 2> touchIds_{-1, -1};
    uint8_t nextTouchId_ = 0;

    std::mutex fbMutex_;
    std::array<uint8_t, 2> rumble_{0, 0};
    std::array<uint8_t, 3> led_{0, 0, 64};
    struct { uint8_t left[11]; uint8_t right[11]; } triggers_{};
    bool rumbleDirty_ = false, ledDirty_ = false, triggersDirty_ = false;

    mutable std::mutex nameMutex_;
    std::string name_;
};

} // namespace

std::unique_ptr<IInputDevice> createSdlGamepad() { return std::make_unique<SdlGamepad>(); }

} // namespace bv::remote

#else

namespace bv::remote {
std::unique_ptr<IInputDevice> createSdlGamepad() { return nullptr; }
} // namespace bv::remote

#endif
