// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_force_feedback.cpp
//
// Minimal, deliberately conservative I-Force effect uploader.

#include "iforce_force_feedback.h"

#include <algorithm>

namespace iforce {
namespace {

    uint8_t high_byte(uint16_t value) {
        return static_cast<uint8_t>(value >> 8);
    }

    uint8_t low_byte(uint16_t value) {
        return static_cast<uint8_t>(value & 0xFF);
    }

    uint8_t signed_level_byte(int16_t level) {
        // This is the HIFIX80 conversion used by the Linux iforce driver.
        return static_cast<uint8_t>((level < 0 ? level + 255 : level) >> 8);
    }

    std::vector<uint8_t> effect_core(uint8_t effect_id, uint8_t effect_type,
        uint8_t axes, uint16_t duration,
        uint16_t modifier1, uint16_t modifier2) {
        return {
            effect_id,
            effect_type,
            axes,
            low_byte(duration),
            high_byte(duration),
            0,
            0,
            0,
            low_byte(modifier1),
            high_byte(modifier1),
            low_byte(modifier2),
            high_byte(modifier2),
            0,
            0,
        };
    }

} // namespace

bool IForceFeedback::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_)
        return true;

    std::vector<uint8_t> response;
    if (!device_.query('O', response)) {
        set_error("I-Force ready query failed: " + device_.last_error());
        return false;
    }
    if (!device_.query('B', response) || response.size() < 3) {
        set_error("I-Force memory query failed: " + device_.last_error());
        return false;
    }

    device_memory_end_ = static_cast<uint16_t>(response[1])
        | (static_cast<uint16_t>(response[2]) << 8);
    if (device_memory_end_ < kMinimumMemory) {
        set_error("I-Force device reports too little effect memory");
        return false;
    }

    // Disable the device's built-in centering before installing our damper.
    if (!send_command(kCmdAutocenter, { 0x03, 0x00 })
        || !send_command(kCmdAutocenter, { 0x04, 0x01 })
        || !send_command(kCmdEnable, { 0x04 })) {
        return false;
    }

    if (!install_damper() || !install_impact_effect()) {
        send_command(kCmdEnable, { 0x01 });
        return false;
    }

    if (!send_command(kCmdPlay, { kDamperEffectId, 0x01, 0x01 })) {
        send_command(kCmdEnable, { 0x01 });
        return false;
    }

    damper_playing_ = true;
    enabled_ = true;
    last_error_.clear();
    return true;
}

void IForceFeedback::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    stop_effect(kImpactEffectId);
    stop_effect(kDamperEffectId);
    send_command(kCmdEnable, { 0x01 });
    enabled_ = false;
    damper_playing_ = false;
    impact_playing_ = false;
}

void IForceFeedback::on_rumble(uint8_t large_motor, uint8_t small_motor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    const uint16_t combined = static_cast<uint16_t>(large_motor) * 3
        + static_cast<uint16_t>(small_motor);
    const int16_t level = static_cast<int16_t>(std::min<uint16_t>(combined * 64, 12000));
    if (level == 0) {
        if (impact_playing_)
            stop_effect(kImpactEffectId);
        impact_playing_ = false;
        return;
    }

    // XInput does not carry direction. Alternating polarity avoids a permanent
    // steering bias while still producing a distinct impact impulse.
    positive_impact_ = !positive_impact_;
    const int16_t signed_level = positive_impact_ ? level : -level;
    if (!send_command(kCmdMagnitude, {
                                         low_byte(kImpactMagnitudeModifier),
                                         high_byte(kImpactMagnitudeModifier),
                                         signed_level_byte(signed_level),
                                     })) {
        return;
    }
    if (!send_command(kCmdPlay, { kImpactEffectId, 0x01, 0x01 }))
        return;
    impact_playing_ = true;
}

bool IForceFeedback::send_command(uint16_t command,
    const std::vector<uint8_t>& data) {
    if (data.size() != static_cast<std::size_t>(command & 0xFF)
        || data.size() + 1 > EP_MAX_PACKET) {
        set_error("invalid I-Force command payload length");
        return false;
    }

    // iforce-usb.c transmits the high command byte followed by the payload;
    // the low command byte is the payload length and is not put on the wire.
    std::vector<uint8_t> packet;
    packet.reserve(data.size() + 1);
    packet.push_back(static_cast<uint8_t>(command >> 8));
    packet.insert(packet.end(), data.begin(), data.end());
    if (!device_.write(packet)) {
        set_error(device_.last_error());
        return false;
    }
    return true;
}

bool IForceFeedback::install_damper() {
    const auto condition = [this](uint16_t address) {
        return send_command(kCmdCondition, {
                                               low_byte(address),
                                               high_byte(address),
                                               25,
                                               25,
                                               0,
                                               0,
                                               0,
                                               0,
                                               100,
                                               100,
                                           });
    };

    if (!condition(kDamperModifier1) || !condition(kDamperModifier2))
        return false;

    return send_command(kCmdEffect,
        effect_core(kDamperEffectId, 0x41, 0xC0, 0xFFFF,
            kDamperModifier1, kDamperModifier2));
}

bool IForceFeedback::install_impact_effect() {
    if (!send_command(kCmdMagnitude, {
                                         low_byte(kImpactMagnitudeModifier),
                                         high_byte(kImpactMagnitudeModifier),
                                         0,
                                     })) {
        return false;
    }

    if (!send_command(kCmdEnvelope, {
                                        low_byte(kImpactEnvelopeModifier),
                                        high_byte(kImpactEnvelopeModifier),
                                        0,
                                        0,
                                        0,
                                        100,
                                        0,
                                        0,
                                    })) {
        return false;
    }

    return send_command(kCmdEffect,
        effect_core(kImpactEffectId, 0x00, 0x20, 250,
            kImpactMagnitudeModifier, kImpactEnvelopeModifier));
}

void IForceFeedback::stop_effect(uint8_t effect_id) {
    send_command(kCmdPlay, { effect_id, 0x00, 0x00 });
}

void IForceFeedback::set_error(const std::string& message) {
    last_error_ = message.empty() ? "I-Force command failed" : message;
}

} // namespace iforce
