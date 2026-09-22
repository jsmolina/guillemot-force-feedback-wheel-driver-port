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

    // XInput motor magnitudes are 0..255, but the I-Force protocol's safe
    // working range for these effect bytes is 0..0x7F (iforce.h's HIFIX80
    // comment: 0x80 is a special value some firmware revisions mishandle).
    // Scale rather than clamp -- clamping flattens every input above 127 to
    // the same force, so the whole top half of the rumble range feels
    // identical.
    uint8_t motor_to_magnitude_byte(uint8_t motor) {
        return static_cast<uint8_t>((static_cast<uint16_t>(motor) * 0x7F) / 0xFF);
    }

    // Effect-core direction, as the high byte of Linux FF's 16-bit polar
    // angle (make_core: data[5] = HI(direction)). With axes = 0x20 the device
    // resolves force from this angle, and 0x0000 ("north") projects to zero
    // on a wheel's single X axis -- the effect plays and reports playing=1
    // while producing no felt force. 0x4000 is 90 degrees: pure +X.
    constexpr uint8_t kDirectionX = 0x40;

    std::vector<uint8_t> effect_core(uint8_t effect_id, uint8_t effect_type,
        uint8_t axes, uint16_t duration,
        uint16_t modifier1, uint16_t modifier2) {
        return {
            effect_id,
            effect_type,
            axes,
            low_byte(duration),
            high_byte(duration),
            kDirectionX,
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

    const uint8_t large_magnitude = motor_to_magnitude_byte(large_motor);
    const uint8_t small_magnitude = motor_to_magnitude_byte(small_motor);

    // --- Continuous rumble bed: update each periodic channel's magnitude
    // live. Skip the write if nothing changed, so idle/steady rumble
    // doesn't spam the interrupt OUT endpoint. Also throttle to one
    // PERIOD update per ~20ms per channel so the firmware has time to
    // absorb the previous update before we overwrite it.
    const auto now = std::chrono::steady_clock::now();
    if (large_magnitude != last_large_magnitude_) {
        if (now - last_large_period_send_ >= kMinPeriodicUpdateInterval) {
            if (update_periodic_magnitude(kLargeMotorPeriodModifier,
                    kLargeMotorPeriodMs, large_magnitude)) {
                last_large_magnitude_ = large_magnitude;
                last_large_period_send_ = now;
                consecutive_write_failures_ = 0;
            } else {
                if (++consecutive_write_failures_ == 1
                    || consecutive_write_failures_ % 20 == 0) {
                    set_error("PERIOD update for large motor failed: "
                        + device_.last_error());
                }
                // On a write failure we intentionally do NOT update
                // last_large_magnitude_: the next on_rumble() call will
                // retry the same value, which is what we want until the
                // endpoint recovers.
            }
        }
    }

    if (small_magnitude != last_small_magnitude_) {
        if (now - last_small_period_send_ >= kMinPeriodicUpdateInterval) {
            if (update_periodic_magnitude(kSmallMotorPeriodModifier,
                    kSmallMotorPeriodMs, small_magnitude)) {
                last_small_magnitude_ = small_magnitude;
                last_small_period_send_ = now;
                consecutive_write_failures_ = 0;
            } else {
                // Note: ++ on the same counter as the large-motor path so
                // the log throttle reflects total failures across both
                // channels, not per-channel.
                if (++consecutive_write_failures_ == 1
                    || consecutive_write_failures_ % 20 == 0) {
                    set_error("PERIOD update for small motor failed: "
                        + device_.last_error());
                }
            }
        }
    }

    // --- Transient impact pulse: fire once per rising edge above
    // threshold, not on every callback while rumble stays high.
    const uint16_t combined = static_cast<uint16_t>(large_motor) * 3
        + static_cast<uint16_t>(small_motor);
    // Cap at 0x7F00 so HIFIX80 yields 0x7F -- the top of the protocol's safe
    // byte range -- when both motors are maxed. combined maxes at 1020, so
    // the *32 scale reaches the cap exactly at full rumble.
    const int16_t level = static_cast<int16_t>(std::min<uint16_t>(combined * 32, 0x7F00));

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
        set_error("impact MAGNITUDE update failed: " + device_.last_error());
        return;
    }
    if (!play_effect(kImpactEffectId, 1)) {
        set_error("impact PLAY failed: " + device_.last_error());
    }
}

void IForceFeedback::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_rearm_ < kEffectRearmInterval)
        return;

    // Safety net only. install_periodic_channel() now starts these channels
    // with a 255x repeat count (~4.6 hours), so this should never actually be
    // needed. It stays because a firmware revision that ignores the 0x41
    // repeat mode would otherwise go silent after 65.5s with no recovery.
    // Once 0x41 is confirmed on real hardware, tick() can be deleted.
    play_effect(kLargeMotorEffectId, kPeriodicRepeatCount);
    play_effect(kSmallMotorEffectId, kPeriodicRepeatCount);
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

bool IForceFeedback::play_effect(uint8_t effect_id, uint8_t repeat_count) {
    const uint8_t mode = repeat_count == 0
        ? 0x00
        : (repeat_count > 1 ? 0x41 : 0x01);
    return send_command(kCmdPlay, { effect_id, mode, repeat_count });
}

bool IForceFeedback::set_magnitude_modifier(uint16_t modifier_address,
    int16_t level) {
    return send_command(kCmdMagnitude, {
                                           low_byte(modifier_address),
                                           high_byte(modifier_address),
                                           signed_level_byte(level),
                                       });
}

bool IForceFeedback::set_period_modifier(uint16_t modifier_address,
    int16_t magnitude, int16_t offset, uint16_t period_ms, uint16_t phase) {
    return send_command(kCmdPeriod, {
                                        low_byte(modifier_address),
                                        high_byte(modifier_address),
                                        signed_level_byte(magnitude),
                                        signed_level_byte(offset),
                                        static_cast<uint8_t>(phase >> 8),
                                        low_byte(period_ms),
                                        high_byte(period_ms),
                                    });
}

bool IForceFeedback::set_envelope_modifier(uint16_t modifier_address,
    uint16_t attack_duration_ms, int16_t initial_level,
    uint16_t fade_duration_ms, int16_t final_level) {
    return send_command(kCmdEnvelope, {
                                          low_byte(modifier_address),
                                          high_byte(modifier_address),
                                          low_byte(attack_duration_ms),
                                          high_byte(attack_duration_ms),
                                          static_cast<uint8_t>(initial_level >> 8),
                                          low_byte(fade_duration_ms),
                                          high_byte(fade_duration_ms),
                                          static_cast<uint8_t>(final_level >> 8),
                                      });
}

bool IForceFeedback::install_impact_effect() {
    if (!set_magnitude_modifier(kImpactMagnitudeModifier, 0))
        return false;

    if (!set_envelope_modifier(kImpactEnvelopeModifier,
            0, 0,
            100, 0)) {
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

    // The Linux driver handles periodic upload as a period modifier + optional
    // envelope block. We keep the device-centric upload path but preserve the
    // default centering behavior and avoid the unsafe zero-FF sentinel.
    if (!set_period_modifier(modifier_address, 0, 0, period_ms, 0))
        return false;

    if (!set_envelope_modifier(envelope_address,
            0, 0,
            100, 0)) {
        return false;
    }

    if (!send_command(kCmdEffect,
            effect_core(effect_id, wave_code, 0x20, 0xFFFF,
                modifier_address, envelope_address))) {
        return false;
    }

    return play_effect(effect_id, kPeriodicRepeatCount);
}

bool IForceFeedback::update_periodic_magnitude(uint16_t modifier_address,
    uint16_t period_ms, uint8_t magnitude) {
    return set_period_modifier(modifier_address,
        static_cast<int16_t>(magnitude) * 256,
        0, period_ms, 0);
}

void IForceFeedback::stop_effect(uint8_t effect_id) {
    play_effect(effect_id, 0);
}

void IForceFeedback::set_error(const std::string& message) {
    last_error_ = message.empty() ? "I-Force command failed" : message;
}

} // namespace iforce
