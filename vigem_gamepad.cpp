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
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <Windows.h>
#endif

#include "vigem_gamepad.h"

#include <ViGEmClient.h>

#include <cstring>

namespace iforce {

VigemGamepad::~VigemGamepad() {
    disconnect();
}

VigemGamepad::VigemGamepad(VigemGamepad&& other) noexcept
    : client_(other.client_), target_(other.target_), last_error_(std::move(other.last_error_)) {
    other.client_ = nullptr;
    other.target_ = nullptr;
}

VigemGamepad& VigemGamepad::operator=(VigemGamepad&& other) noexcept {
    if (this != &other) {
        disconnect();
        client_ = other.client_;
        target_ = other.target_;
        last_error_ = std::move(other.last_error_);
        other.client_ = nullptr;
        other.target_ = nullptr;
    }
    return *this;
}

bool VigemGamepad::connect() {
    if (is_connected()) {
        return true;  // already up
    }

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

    return true;
}

bool VigemGamepad::update(const DeviceState& state) {
    if (!is_connected()) {
        last_error_ = "VigemGamepad::update() called while not connected";
        return false;
    }

    XUSB_REPORT report{};
    report.wButtons = 0;

    // ----- Buttons --------------------------------------------------------
    // Map the iforce button bitmap (16 bits, kernel btn_wheel[] order) onto
    // the Xbox 360 button set.
    //
    //   iforce bit | meaning                  | X360 button
    //   -----------+--------------------------+----------------------------
    //      0       | gear down / paddle L1   | XUSB_GAMEPAD_LEFT_SHOULDER
    //      1       | gear up   / paddle R1   | XUSB_GAMEPAD_RIGHT_SHOULDER
    //      2       | generic BTN_1            | XUSB_GAMEPAD_A
    //      3       | generic BTN_2            | XUSB_GAMEPAD_B
    //      4       | generic BTN_3            | XUSB_GAMEPAD_X
    //      5       | generic BTN_4            | XUSB_GAMEPAD_Y
    //      6       | generic BTN_5            | XUSB_GAMEPAD_BACK
    //      7       | generic BTN_6            | XUSB_GAMEPAD_START
    //      8..13   | generic BTN_7..BTN_12    | XUSB_GAMEPAD_LEFT_THUMB +
    //                                          | XUSB_GAMEPAD_RIGHT_THUMB +
    //                                          | XUSB_GAMEPAD_GUIDE + 3 spares
    //                                          | (we map to A/B/X/Y duplicates
    //                                          |  for the unused ones to avoid
    //                                          |  losing inputs)
    //      14      | BTN_THUMBL               | XUSB_GAMEPAD_LEFT_THUMB
    //      15      | BTN_THUMBR               | XUSB_GAMEPAD_RIGHT_THUMB
    //
    const auto b = [&state](int bit) -> bool {
        return (state.buttons & (1u << bit)) != 0;
    };

    if (b(0))  report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b(1))  report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b(2))  report.wButtons |= XUSB_GAMEPAD_A;
    if (b(3))  report.wButtons |= XUSB_GAMEPAD_B;
    if (b(4))  report.wButtons |= XUSB_GAMEPAD_X;
    if (b(5))  report.wButtons |= XUSB_GAMEPAD_Y;
    if (b(6))  report.wButtons |= XUSB_GAMEPAD_BACK;
    if (b(7))  report.wButtons |= XUSB_GAMEPAD_START;
    if (b(8))  report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b(9))  report.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (b(10)) report.wButtons |= XUSB_GAMEPAD_GUIDE;
    // bits 11..13 have no canonical X360 counterpart; map them onto the
    // remaining thumb/shoulder slots so users still see *something* move.
    if (b(11)) report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b(12)) report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b(13)) report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b(14)) report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b(15)) report.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;

    // ----- D-pad (from hat 0) -------------------------------------------
    const HatXY hat = hat_to_xy(state.hat0);
    if (hat.x < 0) report.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (hat.x > 0) report.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (hat.y < 0) report.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (hat.y > 0) report.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;

    // ----- Axes ---------------------------------------------------------
    // Steering wheel  -> left thumb X  (range -32768..+32767)
    // Gas pedal       -> right trigger  (range 0..255)
    // Brake pedal     -> left trigger   (range 0..255)
    //
    // The iforce wheel reports signed bytes (-128..+127).  We map that range
    // onto the X360's signed 16-bit thumb range by left-shifting 8 bits and
    // duplicating the sign bit (which sign-extension does for free).
    report.sThumbLX = static_cast<int16_t>(static_cast<int16_t>(state.wheel) << 8);
    report.sThumbLY = 0;

    // Pedals: iforce reports them as signed bytes too.  On real hardware
    // they swing from 0 (released) to -128 (fully pressed) because of how
    // the pedals' potentiometer is wired - we mirror the kernel's behaviour
    // of treating "more negative == more pressure".
    auto pedal_to_trigger = [](int8_t v) -> uint8_t {
        // map -128..0 -> 255..0, anything positive -> 0
        if (v >= 0) return 0;
        return static_cast<uint8_t>(-v * 2);   // -1 -> 2, -128 -> 256 clamped to 255
    };

    uint8_t gas = pedal_to_trigger(state.gas_pedal);
    if (gas > 255) gas = 255;
    uint8_t brake = pedal_to_trigger(state.brake_pedal);
    if (brake > 255) brake = 255;

    report.bRightTrigger = gas;
    report.bLeftTrigger  = brake;

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
