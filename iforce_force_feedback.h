// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_force_feedback.h
//
// Conservative force-feedback bridge for the Guillemot I-Force wheel.

#pragma once

#include "usb_device.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace iforce {

class IForceFeedback {
public:
    explicit IForceFeedback(UsbDevice& device)
        : device_(device) {}

    IForceFeedback(const IForceFeedback&) = delete;
    IForceFeedback& operator=(const IForceFeedback&) = delete;

    // Queries device memory, installs a low-strength damper, and enables FF.
    bool initialize();
    void shutdown();

    // Translate XInput motor magnitudes into a short, alternating impact pulse.
    void on_rumble(uint8_t large_motor, uint8_t small_motor);

    bool is_enabled() const { return enabled_; }
    const std::string& last_error() const { return last_error_; }

private:
    static constexpr uint16_t kCmdEffect = 0x010E;
    static constexpr uint16_t kCmdEnvelope = 0x0208;
    static constexpr uint16_t kCmdMagnitude = 0x0303;
    static constexpr uint16_t kCmdCondition = 0x050A;
    static constexpr uint16_t kCmdAutocenter = 0x4002;
    static constexpr uint16_t kCmdPlay = 0x4103;
    static constexpr uint16_t kCmdEnable = 0x4201;

    static constexpr uint8_t kDamperEffectId = 1;
    static constexpr uint8_t kImpactEffectId = 0;
    static constexpr uint16_t kDamperModifier1 = 0;
    static constexpr uint16_t kDamperModifier2 = 8;
    static constexpr uint16_t kImpactMagnitudeModifier = 16;
    static constexpr uint16_t kImpactEnvelopeModifier = 18;
    static constexpr uint16_t kMinimumMemory = 32;

    bool send_command(uint16_t command, const std::vector<uint8_t>& data);
    bool install_damper();
    bool install_impact_effect();
    void stop_effect(uint8_t effect_id);
    void set_error(const std::string& message);

    UsbDevice& device_;
    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool damper_playing_ = false;
    bool impact_playing_ = false;
    bool positive_impact_ = false;
    uint16_t device_memory_end_ = 0;
    std::string last_error_;
};

} // namespace iforce
