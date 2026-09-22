// SPDX-License-Identifier: GPL-2.0-or-later
//
// vigem_gamepad.cpp
//
// Implementation of the ViGEmClient Xbox 360 wrapper.
//
// ViGEmClient headers prefer C linkage and assume Windows headers are
// already included.  We include <Windows.h> here, before the ViGEm header,
// to avoid pulling Windows-specific noise into the rest of the project.

#ifdef _WIN32
#include <Windows.h>
#endif

#include "vigem_gamepad.h"

#include <ViGEm/Client.h>

#include <utility>

namespace iforce {

#ifdef _WIN32
#define IFORCE_VIGEM_CALLBACK CALLBACK
#else
#define IFORCE_VIGEM_CALLBACK
#endif

static void IFORCE_VIGEM_CALLBACK rumble_notification(
    PVIGEM_CLIENT,
    PVIGEM_TARGET,
    UCHAR large_motor,
    UCHAR small_motor,
    UCHAR,
    PVOID user_data) {
    auto* gamepad = static_cast<VigemGamepad*>(user_data);
    gamepad->handle_rumble(large_motor, small_motor);
}

VigemGamepad::~VigemGamepad() {
    disconnect();
}

VigemGamepad::VigemGamepad(VigemGamepad&& other) noexcept
    : client_(other.client_),
      target_(other.target_),
      rumble_callback_(std::move(other.rumble_callback_)),
      last_error_(std::move(other.last_error_)) {
    other.client_ = nullptr;
    other.target_ = nullptr;
    rebind_notification();
}

VigemGamepad& VigemGamepad::operator=(VigemGamepad&& other) noexcept {
    if (this != &other) {
        disconnect();
        client_ = other.client_;
        target_ = other.target_;
        rumble_callback_ = std::move(other.rumble_callback_);
        last_error_ = std::move(other.last_error_);
        other.client_ = nullptr;
        other.target_ = nullptr;
        rebind_notification();
    }
    return *this;
}

bool VigemGamepad::connect(RumbleCallback rumble_callback) {
    if (is_connected()) {
        return true; // already up
    }

    rumble_callback_ = std::move(rumble_callback);

    // 1. Allocate + connect the ViGEm client.
    client_ = vigem_alloc();
    if (client_ == nullptr) {
        last_error_ = "vigem_alloc() returned NULL (out of memory?)";
        return false;
    }

    const VIGEM_ERROR conn_rc = vigem_connect(client_);
    if (!VIGEM_SUCCESS(conn_rc)) {
        last_error_ = std::string("vigem_connect failed (code=")
            + std::to_string(static_cast<int>(conn_rc))
            + "). Is the ViGEmBus driver installed?";
        vigem_free(client_);
        client_ = nullptr;
        return false;
    }

    // 2. Allocate an Xbox 360 target and add it to the bus.
    target_ = vigem_target_x360_alloc();
    if (target_ == nullptr) {
        last_error_ = "vigem_target_x360_alloc() returned NULL";
        vigem_disconnect(client_);
        vigem_free(client_);
        client_ = nullptr;
        return false;
    }

    const VIGEM_ERROR add_rc = vigem_target_add(client_, target_);
    if (!VIGEM_SUCCESS(add_rc)) {
        last_error_ = std::string("vigem_target_add failed (code=")
            + std::to_string(static_cast<int>(add_rc)) + ")";
        vigem_target_free(target_);
        target_ = nullptr;
        vigem_disconnect(client_);
        vigem_free(client_);
        client_ = nullptr;
        return false;
    }

    if (rumble_callback_) {
        const VIGEM_ERROR notify_rc = vigem_target_x360_register_notification(
            client_, target_, rumble_notification, this);
        if (!VIGEM_SUCCESS(notify_rc)) {
            // Deliberately non-fatal: buttons/axes still work without
            // rumble. But this is the one failure mode where everything
            // downstream (FF init, packet framing, USB writes) can look
            // completely correct while force feedback simply never fires,
            // because handle_rumble() was never actually wired up on the
            // ViGEm side. Surface it via last_error() so the caller can
            // log it instead of it disappearing silently.
            last_error_ = std::string("vigem_target_x360_register_notification failed (code=")
                + std::to_string(static_cast<int>(notify_rc))
                + "). Rumble/force-feedback notifications will not be delivered.";
        }
    }

    return true;
}

void VigemGamepad::rebind_notification() {
    if (target_ == nullptr)
        return;
    // See the declaration comment: re-point the SDK's registration at the
    // (possibly new, post-move) `this` instead of leaving it bound to
    // whatever address happened to call connect() originally.
    vigem_target_x360_unregister_notification(target_);
    if (rumble_callback_) {
        const VIGEM_ERROR notify_rc = vigem_target_x360_register_notification(
            client_, target_, rumble_notification, this);
        if (!VIGEM_SUCCESS(notify_rc)) {
            last_error_ = std::string("vigem_target_x360_register_notification failed after move (code=")
                + std::to_string(static_cast<int>(notify_rc)) + ").";
        }
    }
}

void VigemGamepad::handle_rumble(uint8_t large_motor, uint8_t small_motor) {
    if (rumble_callback_)
        rumble_callback_(large_motor, small_motor);
}

bool VigemGamepad::update(const DeviceState& state) {
    if (!is_connected()) {
        last_error_ = "VigemGamepad::update() called while not connected";
        return false;
    }

    XUSB_REPORT report{};
    report.wButtons = 0;

    // ----- Buttons --------------------------------------------------------
    // Map the iforce button bitmap (kernel btn_wheel[] order) onto
    // the Xbox 360 button set.
    //
    // Bit assignments below were measured on the physical 06f8:0004 wheel by
    // pressing each control and reading the raw bitmap -- they are not the
    // kernel's btn_wheel[] guesses, which had the two paddles the wrong way
    // round.
    //
    // The gear stick takes the shoulders because that is where racing games
    // put their default shift bindings; the paddles land on X/Y.
    //
    // The right hat is split across both bytes: its left/down directions are
    // bits in the hat1 nibble, while its up/right directions are bits 6 and 7
    // of the button byte. No nibble-wide decoder can express that, so it is
    // handled as four independent buttons below. This exhausts the wheel's
    // controls, so every X360 digital button is driven exactly once.
    //
    //   source            | measured control    | X360 button
    //   ------------------+---------------------+-------------------------
    //   buttons bit 0     | right paddle        | XUSB_GAMEPAD_Y
    //   buttons bit 1     | left paddle         | XUSB_GAMEPAD_X
    //   buttons bit 2     | right face button   | XUSB_GAMEPAD_START
    //   buttons bit 3     | left face button    | XUSB_GAMEPAD_BACK
    //   buttons bit 4     | gear up             | XUSB_GAMEPAD_RIGHT_SHOULDER
    //   buttons bit 5     | gear down           | XUSB_GAMEPAD_LEFT_SHOULDER
    //   buttons bit 6     | right hat up        | XUSB_GAMEPAD_LEFT_THUMB
    //   buttons bit 7     | right hat right     | XUSB_GAMEPAD_B
    //   hat1    bit 0     | right hat down      | XUSB_GAMEPAD_RIGHT_THUMB
    //   hat1    bit 1     | right hat left      | XUSB_GAMEPAD_A
    const auto b = [&state](int bit) -> bool {
        return (state.buttons & (1u << bit)) != 0;
    };
    const auto h1 = [&state](int bit) -> bool {
        return (state.hat1 & (1u << bit)) != 0;
    };

    if (b(0))
        report.wButtons |= XUSB_GAMEPAD_Y;
    if (b(1))
        report.wButtons |= XUSB_GAMEPAD_X;
    if (b(2))
        report.wButtons |= XUSB_GAMEPAD_START;
    if (b(3))
        report.wButtons |= XUSB_GAMEPAD_BACK;
    if (b(4))
        report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b(5))
        report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b(right_hat::kUpBitInButtons))
        report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b(right_hat::kRightBitInButtons))
        report.wButtons |= XUSB_GAMEPAD_B;
    if (h1(right_hat::kDownBitInHat1))
        report.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (h1(right_hat::kLeftBitInHat1))
        report.wButtons |= XUSB_GAMEPAD_A;
    // ----- D-pad (from hat 0) -------------------------------------------
    const HatXY hat = hat_to_xy(state.hat0);
    if (hat.x < 0)
        report.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (hat.x > 0)
        report.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (hat.y < 0)
        report.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (hat.y > 0)
        report.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;

    // ----- Axes ---------------------------------------------------------
    // Steering wheel  -> left thumb X  (kernel range -1920..+1920)
    // Gas pedal       -> right trigger  (range 0..255)
    // Brake pedal     -> left trigger   (range 0..255)
    //
    constexpr int kKernelWheelMax = 1920;
    int wheel = static_cast<int>(state.wheel) * 32767 / kKernelWheelMax;
    if (wheel < -32768)
        wheel = -32768;
    if (wheel > 32767)
        wheel = 32767;
    report.sThumbLX = static_cast<int16_t>(wheel);
    report.sThumbLY = 0;

    report.bRightTrigger = state.gas_pedal;
    report.bLeftTrigger = state.brake_pedal;

    // Right thumb: unused on a wheel - centre it.
    report.sThumbRX = 0;
    report.sThumbRY = 0;

    const VIGEM_ERROR rc = vigem_target_x360_update(client_, target_, report);
    if (!VIGEM_SUCCESS(rc)) {
        last_error_ = std::string("vigem_target_x360_update failed (code=")
            + std::to_string(static_cast<int>(rc)) + ")";
        return false;
    }

    last_error_.clear();
    return true;
}

void VigemGamepad::disconnect() {
    if (target_ != nullptr) {
        if (client_ != nullptr) {
            vigem_target_x360_unregister_notification(target_);
            vigem_target_remove(client_, target_);
        }
        vigem_target_free(target_);
        target_ = nullptr;
    }
    if (client_ != nullptr) {
        vigem_disconnect(client_);
        vigem_free(client_);
        client_ = nullptr;
    }
}

} // namespace iforce
