// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_force_feedback.cpp
//
// I-Force effect uploader: one FF_CONSTANT impact pulse plus two
// continuous FF_PERIODIC channels driving LargeMotor/SmallMotor.

#include "iforce_force_feedback.h"

#include <algorithm>
#include <thread>

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

    // XInput motor magnitudes are 0..255. Map to a device magnitude byte
    // in 0..0x7F: the iforce.h HIFIX80 comment notes 0x80 is a value some
    // firmwares mishandle, so staying at or below 0x7F avoids it entirely.
    uint8_t motor_to_magnitude_byte(uint8_t motor) {
        return static_cast<uint8_t>(motor >> 1);
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

    // Retry the readiness query like iforce_init_device() does: real
    // hardware commonly doesn't answer the very first control transfer
    // right after the interface is claimed (up to ~5s, 20 attempts).
    std::vector<uint8_t> response;
    bool ready = false;
    for (int attempt = 0; attempt < 20 && !ready; ++attempt) {
        ready = device_.query('O', response);
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!ready) {
        set_error("I-Force ready query timed out after 5s: " + device_.last_error());
        return false;
    }

    // 'B' failure is only a warning in the kernel driver, not fatal --
    // it falls back to a default memory size and keeps going.
    device_memory_end_ = 200;
    if (device_.query('B', response) && response.size() >= 3) {
        device_memory_end_ = static_cast<uint16_t>(response[1])
            | (static_cast<uint16_t>(response[2]) << 8);
    }
    if (device_memory_end_ < kMinimumMemory) {
        set_error("I-Force device reports too little effect memory");
        return false;
    }

    // Re-enable the wheel's built-in centering spring using the Linux
    // iforce packet encoding: the second byte is the encoded strength,
    // not a 0..100 percentage value.
    if (!send_command(kCmdAutocenter, { 0x03, kAutocenterStrength })
        || !send_command(kCmdAutocenter, { 0x04, 0x01 })
        || !send_command(kCmdEnable, { 0x04 })) {
        return false;
    }

    if (!install_impact_effect()
        || !install_periodic_channel(kLargeMotorEffectId, kWaveSine,
            kLargeMotorPeriodMs, kLargeMotorPeriodModifier)
        || !install_periodic_channel(kSmallMotorEffectId, kWaveSquare,
            kSmallMotorPeriodMs, kSmallMotorPeriodModifier)) {
        send_command(kCmdEnable, { 0x01 });
        return false;
    }

    last_large_magnitude_ = 0xFF;
    last_small_magnitude_ = 0xFF;
    was_above_threshold_ = false;
    last_rearm_ = std::chrono::steady_clock::now();

    enabled_ = true;
    last_error_.clear();
    return true;
}

void IForceFeedback::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    stop_effect(kImpactEffectId);
    stop_effect(kLargeMotorEffectId);
    stop_effect(kSmallMotorEffectId);
    send_command(kCmdEnable, { 0x01 });
    enabled_ = false;
}

void IForceFeedback::on_rumble(uint8_t large_motor, uint8_t small_motor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    // --- Continuous rumble bed: update each periodic channel's magnitude
    // live. Skip the write if nothing changed, so idle/steady rumble
    // doesn't spam the interrupt OUT endpoint.
    const uint8_t large_magnitude = motor_to_magnitude_byte(large_motor);
    if (large_magnitude != last_large_magnitude_) {
        if (update_periodic_magnitude(kLargeMotorPeriodModifier,
                kLargeMotorPeriodMs, large_magnitude)) {
            last_large_magnitude_ = large_magnitude;
        }
    }

    const uint8_t small_magnitude = motor_to_magnitude_byte(small_motor);
    if (small_magnitude != last_small_magnitude_) {
        if (update_periodic_magnitude(kSmallMotorPeriodModifier,
                kSmallMotorPeriodMs, small_magnitude)) {
            last_small_magnitude_ = small_magnitude;
        }
    }

    // --- Transient impact pulse: fire once per rising edge above
    // threshold, not on every callback while rumble stays high.
    const uint16_t combined = static_cast<uint16_t>(large_motor) * 3
        + static_cast<uint16_t>(small_motor);
    const int16_t level = static_cast<int16_t>(std::min<uint16_t>(combined * 64, 12000));
    const auto now = std::chrono::steady_clock::now();

    if (level < kImpactTriggerThreshold) {
        was_above_threshold_ = false;
        return;
    }
    if (was_above_threshold_
        || now - last_trigger_ < std::chrono::milliseconds(kImpactDurationMs)) {
        return;
    }
    was_above_threshold_ = true;
    last_trigger_ = now;

    positive_impact_ = !positive_impact_;
    const int16_t signed_level = positive_impact_ ? level : -level;
    if (!send_command(kCmdMagnitude, { low_byte(kImpactMagnitudeModifier), high_byte(kImpactMagnitudeModifier), signed_level_byte(signed_level) })) {
        return;
    }
    send_command(kCmdPlay, { kImpactEffectId, 0x01, 0x01 });
}

void IForceFeedback::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_rearm_ < kEffectRearmInterval)
        return;

    // Re-issue Play for the two looping channels so their on-device
    // duration countdown (max ~65.5s) never runs out mid-session.
    send_command(kCmdPlay, { kLargeMotorEffectId, 0x01, 0x01 });
    send_command(kCmdPlay, { kSmallMotorEffectId, 0x01, 0x01 });
    last_rearm_ = now;
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

bool IForceFeedback::install_periodic_channel(uint8_t effect_id,
    uint8_t wave_code, uint16_t period_ms, uint16_t modifier_address) {
    const uint16_t envelope_address = (modifier_address == kLargeMotorPeriodModifier)
        ? kLargeMotorEnvelopeModifier
        : kSmallMotorEnvelopeModifier;

    // Write the period modifier at zero magnitude first (idle) ...
    if (!update_periodic_magnitude(modifier_address, period_ms, 0))
        return false;

    // ... then allocate an inert envelope (attack/fade block) explicitly.
    // Some older I-Force firmware does not handle the protocol "no envelope"
    // 0xFFFF sentinel reliably, so this avoids dereferencing/invalidating it.
    if (!send_command(kCmdEnvelope, {
                                        low_byte(envelope_address),
                                        high_byte(envelope_address),
                                        0,
                                        0,
                                        0,
                                        100,
                                        0,
                                        0,
                                    })) {
        return false;
    }

    if (!send_command(kCmdEffect,
            effect_core(effect_id, wave_code, 0x20, 0xFFFF,
                modifier_address, envelope_address))) {
        return false;
    }

    // Start it. It keeps looping (the waveform itself oscillates) until
    // its duration elapses; tick() re-arms it well before that happens.
    return send_command(kCmdPlay, { effect_id, 0x01, 0x01 });
}

bool IForceFeedback::update_periodic_magnitude(uint16_t modifier_address,
    uint16_t period_ms, uint8_t magnitude) {
    // FF_CMD_PERIOD payload: address, magnitude, offset, phase, period.
    return send_command(kCmdPeriod, {
                                        low_byte(modifier_address),
                                        high_byte(modifier_address),
                                        magnitude,
                                        0, // offset
                                        0, // phase
                                        low_byte(period_ms),
                                        high_byte(period_ms),
                                    });
}

void IForceFeedback::stop_effect(uint8_t effect_id) {
    send_command(kCmdPlay, { effect_id, 0x00, 0x00 });
}

void IForceFeedback::set_error(const std::string& message) {
    last_error_ = message.empty() ? "I-Force command failed" : message;
}

} // namespace iforce
