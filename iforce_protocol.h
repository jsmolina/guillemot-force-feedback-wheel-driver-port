// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_protocol.h
//
// Minimal user-mode port of the Linux kernel's iforce USB protocol
// (drivers/input/joystick/iforce/iforce-usb.c + iforce-main.c + iforce-input.c)
// for vendor 0x06f8 product 0x0004 (Guillemot Force Feedback Racing Wheel).
//
// We only implement reading buttons and axes. Force-feedback output paths
// (FF_CMD_*) are intentionally not ported in this iteration.

#pragma once

#include <cstdint>
#include <array>

namespace iforce {

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------
constexpr uint16_t USB_VENDOR   = 0x06F8;
constexpr uint16_t USB_PRODUCT  = 0x0004;

// USB endpoint addresses taken from the kernel iforce-usb.c driver.
// The Guillemot wheel exposes a single HID-class interface with one IN
// interrupt endpoint on address 0x82 (EP2 IN) and one OUT endpoint on 0x02.
constexpr uint8_t  EP_IN_ADDR   = 0x82;
constexpr uint8_t  EP_OUT_ADDR  = 0x02;
constexpr uint8_t  EP_MAX_PACKET = 16;       // matches IFORCE_MAX_PACKET_LENGTH

// ---------------------------------------------------------------------------
// iforce packet command opcodes (lower byte of the 16-bit "cmd")
// These are the same opcodes used by the kernel driver.
// ---------------------------------------------------------------------------
enum Command : uint8_t {
    CMD_BUTTONS      = 0x01,  // button/axis state update (the main input packet)
    CMD_RESPONSE      = 0x02,  // generic response to a host-initiated request
    CMD_HIGH_LEVEL    = 0x03,  // multi-byte axis / config response
    CMD_LOW_LEVEL     = 0x04,  // 1-byte axis response (one axis value)
};

// ---------------------------------------------------------------------------
// Parsed device state.
//
// Mapping table for the Guillemot Force Feedback Racing Wheel (06f8:0004).
// This mirrors the kernel's btn_wheel[] + abs_wheel[] tables.
//
//   Buttons (16 bits packed in data[0..1]):
//     bit 0 -> Gear down / paddle L1
//     bit 1 -> Gear up   / paddle R1
//     bit 2..13 -> generic BTN_1..BTN_12 on the wheel face
//     bit 14 -> BTN_THUMBL (left stick click on emulated pads)
//     bit 15 -> BTN_THUMBR (right stick click)
//
//   Axes (each one signed byte from the data payload):
//     index 0 -> wheel  X (steering)
//     index 1 -> gas    pedal
//     index 2 -> brake   pedal
//
//   Hat switches (4-bit values, 0..8 = neutral, 1=N, 2=NE, ... 8=NW):
//     index 0 -> hat 0 X/Y  (D-pad on the wheel face)
//     index 1 -> hat 1 X/Y  (often unused on the 0x0004)
// ---------------------------------------------------------------------------
struct DeviceState {
    uint16_t buttons   = 0;     // raw 16-bit button bitmap
    int8_t   wheel     = 0;     // signed -128..+127
    int8_t   gas_pedal = 0;     // signed -128..+127
    int8_t   brake_pedal= 0;    // signed -128..+127
    uint8_t  hat0      = 0;     // 4-bit D-pad code
    uint8_t  hat1      = 0;     // 4-bit D-pad code (usually 0)
};

// Decode one USB interrupt-transfer buffer into a DeviceState.
//
// The iforce USB protocol is *not* framed with 0x02/0x03 markers on the
// wire - those markers are only used on the RS-232 serial variant.  For USB,
// each interrupt URB carries a complete packet directly:
//
//   data[0]  : cmd high byte  (almost always 0x02 = "input report")
//   data[1]  : cmd low  byte   (the Command opcode above)
//   data[2..]: payload bytes
//
// This is exactly the layout consumed by `iforce_process_packet()` in
// iforce-main.c after the USB IRQ handler hands the buffer over.
//
// Returns true if the packet was a recognised input packet and `out`
// was written; false otherwise (caller should ignore).
bool decode_packet(const uint8_t* data, std::size_t length, DeviceState& out);

// Convenience: convert a hat 4-bit code into X/Y signed values in the
// range -1..+1 (compatible with Xbox 360 D-pad encoding).
struct HatXY { int x; int y; };
HatXY hat_to_xy(uint8_t hat_code);

} // namespace iforce
