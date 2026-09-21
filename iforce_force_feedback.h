// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_force_feedback.h
//
// Force-feedback bridge for the Guillemot I-Force wheel.
//
// LargeMotor and SmallMotor from XInput each drive their own continuous
// FF_PERIODIC effect (different waveform/period per motor, so they feel
// distinct rather than being flattened into one generic "buzz"). A rising
// edge of the combined rumble level additionally fires a short FF_CONSTANT
// "impact" pulse, so sharp hits still register as a felt thump on top of
// the ongoing rumble bed.

#pragma once

#include "usb_device.h"

#include <chrono>
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

    // Waits for device readiness, queries effect memory, and installs the
    // impact effect plus the two continuous periodic rumble channels.
    bool initialize();
    void shutdown();

    // Feed in the raw XInput motor magnitudes (0..255 each) on every rumble
    // callback. Updates the two periodic channels' magnitude live, and
    // fires the impact pulse on a rising edge of the combined level.
    void on_rumble(uint8_t large_motor, uint8_t small_motor);

    // Call periodically (e.g. once per main-loop iteration). The I-Force
    // effect "duration" field is 16 bits (max ~65.5s), so a continuously
    // playing periodic effect can silently expire on the device even
    // though nothing on the host side changed. tick() re-arms the two
    // looping channels well before that limit so they never audibly lapse.
    void tick();

    bool is_enabled() const { return enabled_; }
    const std::string& last_error() const { return last_error_; }

private:
    static constexpr uint16_t kCmdEffect = 0x010E;
    static constexpr uint16_t kCmdEnvelope = 0x0208;
    static constexpr uint16_t kCmdMagnitude = 0x0303;
    static constexpr uint16_t kCmdPeriod = 0x0407;
    static constexpr uint16_t kCmdAutocenter = 0x4002;
    static constexpr uint16_t kCmdPlay = 0x4103;
    static constexpr uint16_t kCmdEnable = 0x4201;

    // Device-side effect ids (channels). Each must be distinct so several
    // effects can play at the same time.
    static constexpr uint8_t kImpactEffectId = 0;
    static constexpr uint8_t kLargeMotorEffectId = 1;
    static constexpr uint8_t kSmallMotorEffectId = 2;

    // Device memory layout. Impact needs a magnitude modifier (2B) and an
    // envelope modifier (14B); each periodic channel needs a period modifier
    // (12B) and an inert envelope modifier (14B) because some older I-Force
    // firmware mishandles the protocol's 0xFFFF "no envelope" sentinel.
    static constexpr uint16_t kImpactMagnitudeModifier = 0;
    static constexpr uint16_t kImpactEnvelopeModifier = 2;
    static constexpr uint16_t kLargeMotorPeriodModifier = 16;
    static constexpr uint16_t kLargeMotorEnvelopeModifier = 28;
    static constexpr uint16_t kSmallMotorPeriodModifier = 42;
    static constexpr uint16_t kSmallMotorEnvelopeModifier = 54;
    static constexpr uint16_t kMinimumMemory = 68;

    // Waveform bytes (iforce-ff.c wave_code values): square feels sharp
    // and buzzy, sine feels smoother and deeper.
    static constexpr uint8_t kWaveSquare = 0x20;
    static constexpr uint8_t kWaveSine = 0x22;

    // Chosen so the two motors feel distinct: large = slow/deep rumble,
    // small = fast/buzzy texture. Tune to taste.
    static constexpr uint16_t kLargeMotorPeriodMs = 140; // ~7 Hz
    static constexpr uint16_t kSmallMotorPeriodMs = 45;  // ~22 Hz

    static constexpr std::chrono::seconds kEffectRearmInterval{ 30 };
    static constexpr int kImpactDurationMs = 250;
    static constexpr int16_t kImpactTriggerThreshold = 4000;

    // Linux iforce_set_autocenter() takes a 16-bit magnitude and sends
    // data[1] = magnitude >> 9. For full centering, the single payload byte
    // is 0x7F (127); anything smaller weakens or zeroes the spring.
    static constexpr uint8_t kAutocenterStrength = 0x7F;

    bool send_command(uint16_t command, const std::vector<uint8_t>& data);
    bool install_impact_effect();
    bool install_periodic_channel(uint8_t effect_id, uint8_t wave_code,
        uint16_t period_ms, uint16_t modifier_address);
    bool update_periodic_magnitude(uint16_t modifier_address,
        uint16_t period_ms, uint8_t magnitude);
    void stop_effect(uint8_t effect_id);
    void set_error(const std::string& message);

    UsbDevice& device_;
    mutable std::mutex mutex_;
    bool enabled_ = false;

    // Impact (constant force) transient state.
    bool positive_impact_ = false;
    bool was_above_threshold_ = false;
    std::chrono::steady_clock::time_point last_trigger_{};

    // Periodic rumble-bed state, used to avoid resending identical
    // magnitude packets on every single rumble callback tick.
    uint8_t last_large_magnitude_ = 0xFF; // invalid sentinel forces first send
    uint8_t last_small_magnitude_ = 0xFF;

    // Re-arm bookkeeping for the 16-bit effect duration limit.
    std::chrono::steady_clock::time_point last_rearm_{};

    uint16_t device_memory_end_ = 0;
    std::string last_error_;
};

} // namespace iforce
