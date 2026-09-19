// SPDX-License-Identifier: GPL-2.0-or-later
//
// vigem_gamepad.h
//
// RAII wrapper around the ViGEmClient Xbox 360 virtual gamepad target.
//
// We only feed the read-side (buttons + axes) into the virtual pad - no FF.

#pragma once

#include "iforce_protocol.h"

#include <cstdint>
#include <string>

// Forward-declare ViGEm types without defining the SDK's pointer aliases.
struct _VIGEM_CLIENT_T;
struct _VIGEM_TARGET_T;

namespace iforce {

class VigemGamepad {
public:
    VigemGamepad() = default;
    ~VigemGamepad();

    VigemGamepad(const VigemGamepad&) = delete;
    VigemGamepad& operator=(const VigemGamepad&) = delete;
    VigemGamepad(VigemGamepad&&) noexcept;
    VigemGamepad& operator=(VigemGamepad&&) noexcept;

    // Connect to the ViGEm bus and create an Xbox 360 target.
    // Returns false on failure (ViGEmBus driver not installed, etc.).
    bool connect();

    // Push a new DeviceState into the virtual pad.
    // Returns false on failure (target unplugged, etc.).
    bool update(const DeviceState& state);

    void disconnect();

    const std::string& last_error() const { return last_error_; }
    bool is_connected() const { return target_ != nullptr; }

private:
    _VIGEM_CLIENT_T* client_ = nullptr;
    _VIGEM_TARGET_T* target_ = nullptr;
    std::string last_error_;
};

} // namespace iforce
