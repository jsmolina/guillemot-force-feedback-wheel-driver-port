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
// Mapping table for the Guillemot Force Feedback Racing Wheel (06f8:0004),
// measured on the physical wheel. The kernel's btn_wheel[] table is a guess
// for this PID and gets the two paddles the wrong way round.
//
//   Buttons (8 bits in data[5]):
//     bit 0 -> right paddle shifter
//     bit 1 -> left paddle shifter
//     bit 2 -> right face button
//     bit 3 -> left face button
//     bit 4 -> gear up
//     bit 5 -> gear down
//     bit 6 -> right hat, up
//     bit 7 -> right hat, right
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
//   hat 1 (low nibble) is a bitmask, not an index, and on this wheel it
//   carries only two of the right hat's four directions -- bit0 is down and
//   bit1 is left. Up and right arrive as bits 6 and 7 of the button byte
//   instead. See the right_hat constants below; feeding hat1 to hat_to_xy()
//   yields garbage directions.
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
    uint8_t hat1 = 0;        // right-hat bitmask -> see right_hat below
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

// Right-hat bit positions, measured on the physical 06f8:0004 wheel by
// pressing each direction and dumping the raw report.
//
// The right hat is SPLIT across two bytes: left and down are bits in the
// hat1 nibble, while up and right are the top two bits of the button byte.
// No single-nibble decoder can express this, which is why hat1 has no
// counterpart to hat_to_xy() -- callers test these bits directly.
//
// The kernel's second-hat block does not describe this device: it reads all
// four directions out of the low nibble, and its bit0/bit1 axis signs are
// inverted relative to what this wheel actually reports.
namespace right_hat {
    constexpr uint8_t kDownBitInHat1 = 0;
    constexpr uint8_t kLeftBitInHat1 = 1;
    constexpr uint8_t kUpBitInButtons = 6;
    constexpr uint8_t kRightBitInButtons = 7;
} // namespace right_hat

} // namespace iforce
