// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_protocol.h
//
// Minimal user-mode port of the Linux kernel's iforce USB protocol
// (drivers/input/joystick/iforce/iforce-usb.c + iforce-packets.c)
// for vendor 0x06f8 product 0x0004 (Guillemot Force Feedback Racing Wheel).
//
// Input decoding lives here; force-feedback output encoding is implemented
// separately in iforce_force_feedback.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iforce {

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------
constexpr uint16_t USB_VENDOR = 0x06F8;
constexpr uint16_t USB_PRODUCT = 0x0004;

// USB endpoint addresses taken from the kernel iforce-usb.c driver.
// The Guillemot wheel exposes a single HID-class interface with one IN
// interrupt endpoint on address 0x82 (EP2 IN) and one OUT endpoint on 0x02.
constexpr uint8_t EP_IN_ADDR = 0x82;
constexpr uint8_t EP_OUT_ADDR = 0x02;
constexpr uint8_t EP_MAX_PACKET = 16; // matches IFORCE_MAX_PACKET_LENGTH

// ---------------------------------------------------------------------------
// iforce packet identifiers used by the kernel packet dispatcher.
// ---------------------------------------------------------------------------
enum Command : uint8_t {
    CMD_JOYSTICK = 0x01,
    CMD_STATUS = 0x02,
    CMD_WHEEL = 0x03,
};

// Parsed contents of a 0x02 status report. The device emits these after it
// finishes processing a modifier upload (MAGNITUDE/PERIOD/ENVELOPE/CONDITION)
// so the host can clear its FF_CORE_UPDATE bit (see Linux
// iforce-packets.c:mark_core_as_ready). The Windows port does not gate
// uploads on these reports, but the dispatcher surfaces them so the host
// can log them and (optionally) tune its throttle.
struct StatusReport {
    bool valid = false;
    bool deadman = false;          // data[0] & 0x02
    uint8_t effect_id = 0;         // data[1] & 0x7F (bit 7 = is currently playing)
    bool playing = false;
    // Each (lo,hi) pair after data[1] is the little-endian address of a
    // modifier that the device has finished absorbing.
    std::vector<uint16_t> ready_modifier_addresses;
};

// Decode a 0x02 status report. Returns true if the packet was a status
// report (regardless of whether its contents were well-formed enough to
// populate `out`). Callers that just want to ignore status reports can
// skip this entirely.
bool decode_status_report(const uint8_t* data, std::size_t length, StatusReport& out);

// ---------------------------------------------------------------------------
// Parsed device state.
//
// Mapping table for the Guillemot Force Feedback Racing Wheel (06f8:0004).
// This mirrors the kernel's btn_wheel[] + abs_wheel[] tables.
//
//   Buttons (8 bits in data[5]):
//     bit 0 -> Gear down / paddle L1
//     bit 1 -> Gear up   / paddle R1
//     bit 2..7 -> generic wheel buttons
//
//   Wheel packet payload:
//     data[0..1] -> signed 16-bit wheel position
//     data[2]    -> gas pedal, inverted 0..255
//     data[3]    -> brake pedal, inverted 0..255
//     data[4]    -> unused by the kernel input path
//     data[5]    -> button bitmap (8 buttons: 2 shift paddles + 6 face/base)
//     data[6]    -> hat 0 direction code in the high nibble,
//                   hat 1 direction *bits* in the low nibble
//
// The two hats are NOT encoded the same way -- this is easy to get wrong:
//
//   hat 0 (high nibble) is an index into the 16-entry direction table, so
//   it goes through hat_to_xy().
//
//   hat 1 (low nibble) is a bitmask, not an index. iforce_report_hats_buttons()
//   decodes it as bit3 -> X=-1, bit1 -> X=+1, bit0 -> Y=-1, bit2 -> Y=+1.
//   Feeding it to hat_to_xy() yields garbage directions; use hat1_to_xy().
//
// Note the kernel registers 06f8:0004 with abs_wheel[], which has no
// ABS_HAT1X/ABS_HAT1Y, so upstream never reports the second hat for this
// device. The device table entry is flagged "//?" in iforce-main.c (an
// admitted guess), and this wheel does physically have two hats, so we decode
// it here regardless.
// ---------------------------------------------------------------------------
struct DeviceState {
    uint16_t buttons = 0;    // raw button bitmap
    int16_t wheel = 0;       // signed wheel position
    uint8_t gas_pedal = 0;   // normalized 0..255
    uint8_t brake_pedal = 0; // normalized 0..255
    uint8_t hat0 = 0;        // 4-bit direction code -> hat_to_xy()
    uint8_t hat1 = 0;        // 4-bit direction bitmask -> hat1_to_xy()
};

// Decode one USB interrupt-transfer buffer into a DeviceState.
//
// Each USB interrupt URB carries a complete packet directly:
//
//   data[0]  : packet identifier (the Command enum above)
//   data[1..]: packet payload
//
// This matches the arguments passed to `iforce_process_packet()` by the
// Linux USB interrupt handler.
//
// Returns true if the packet was a recognised *input* packet (0x01 joystick
// or 0x03 wheel) and `out` was written; false otherwise (status reports,
// truncated packets, unsupported ids) — caller should ignore.
bool decode_packet(const uint8_t* data, std::size_t length, DeviceState& out);

// Convenience: convert a hat code into X/Y signed values in the range
// -1..+1 (compatible with Xbox 360 D-pad encoding).
struct HatXY {
    int x;
    int y;
};

// For hat 0 (high nibble of data[6]): table lookup on a direction index.
HatXY hat_to_xy(uint8_t hat_code);

// For hat 1 (low nibble of data[6]): bitmask decode. Mirrors the second-hat
// block of the kernel's iforce_report_hats_buttons().
HatXY hat1_to_xy(uint8_t hat_bits);

} // namespace iforce
