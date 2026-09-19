// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_protocol.h
//
// Minimal user-mode port of the Linux kernel's iforce USB protocol
// (drivers/input/joystick/iforce/iforce-usb.c + iforce-packets.c)
// for vendor 0x06f8 product 0x0004 (Guillemot Force Feedback Racing Wheel).
//
// We only implement reading buttons and axes. Force-feedback output paths
// (FF_CMD_*) are intentionally not ported in this iteration.

#pragma once

#include <cstddef>
#include <cstdint>

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
//     data[5]    -> button bitmap
//     data[6]    -> hat 0 in the high nibble, hat 1 bits in the low nibble
//
//   Hat switches (high nibble of data[6]):
//     index 0 -> hat 0 X/Y  (D-pad on the wheel face)
//     index 1 -> hat 1 X/Y  (often unused on the 0x0004)
// ---------------------------------------------------------------------------
struct DeviceState {
    uint16_t buttons = 0;    // raw button bitmap
    int16_t wheel = 0;       // signed wheel position
    uint8_t gas_pedal = 0;   // normalized 0..255
    uint8_t brake_pedal = 0; // normalized 0..255
    uint8_t hat0 = 0;        // 4-bit D-pad code
    uint8_t hat1 = 0;        // 4-bit D-pad code (usually 0)
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
// Returns true if the packet was a recognised input packet and `out`
// was written; false otherwise (caller should ignore).
bool decode_packet(const uint8_t* data, std::size_t length, DeviceState& out);

// Convenience: convert a hat 4-bit code into X/Y signed values in the
// range -1..+1 (compatible with Xbox 360 D-pad encoding).
struct HatXY {
    int x;
    int y;
};
HatXY hat_to_xy(uint8_t hat_code);

} // namespace iforce
