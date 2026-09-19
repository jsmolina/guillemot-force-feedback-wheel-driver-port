// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_protocol.cpp
//
// Implementation of the iforce USB packet decoder.
//
// The decoder is a 1-to-1 port of the kernel logic found in:
//   - drivers/input/joystick/iforce/iforce-usb.c  (USB IRQ path)
//   - drivers/input/joystick/iforce/iforce-main.c  (cmd dispatch)
//   - drivers/input/joystick/iforce/iforce-input.c (input -> input_event)
//
// We only port the read path (buttons + axes).  All write/FF paths in the
// kernel driver are intentionally omitted from this user-mode port.

#include "iforce_protocol.h"

#include <cstring>

namespace iforce {

// Hat table (same one used by the kernel `iforce_hat_to_xy` helper).
// Index = hat nibble (0..15).  Only indices 0..8 are valid directions,
// 0 means "neutral / centred".  Indices 9..15 fall through to neutral.
static const HatXY kHatTable[16] = {
    /* 0 */ { 0,  0},   // neutral
    /* 1 */ { 0, -1},   // N
    /* 2 */ { 1, -1},   // NE
    /* 3 */ { 1,  0},   // E
    /* 4 */ { 1,  1},   // SE
    /* 5 */ { 0,  1},   // S
    /* 6 */ {-1,  1},   // SW
    /* 7 */ {-1,  0},   // W
    /* 8 */ {-1, -1},   // NW
    /* 9..15 -> neutral fallback */
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}
};

HatXY hat_to_xy(uint8_t hat_code) {
    return kHatTable[hat_code & 0x0F];
}

bool decode_packet(const uint8_t* data, std::size_t length, DeviceState& out) {
    // The kernel only ever routes USB IRQ buffers with >= 2 bytes into
    // iforce_process_packet().  Anything shorter is bogus.
    if (data == nullptr || length < 2) {
        return false;
    }

    // cmd = (data[0] << 8) | data[1]   <-- exactly what iforce_usb_irq() does
    const uint8_t cmd_hi  = data[0];
    const uint8_t cmd_lo  = data[1];
    const uint8_t* payload = data + 2;
    const std::size_t payload_len = length - 2;

    (void)cmd_hi;   // high byte is normally 0x02 for input packets; unused here

    switch (cmd_lo) {
    case CMD_BUTTONS: {
        // The kernel's iforce_input_packet() handles this opcode and reads
        // 16 bits of button state + signed-byte axis values + 4-bit hat
        // codes, in this exact order:
        //
        //   payload[0..1]   : 16 bits of button state
        //   payload[2]      : steering axis (signed byte)
        //   payload[3]      : gas pedal    (signed byte)
        //   payload[4]      : brake pedal  (signed byte)
        //   payload[5] >> 4 : hat 0 code
        //   payload[5] & 0xF: hat 1 code
        //
        if (payload_len < 6) {
            // truncated input packet - skip
            return false;
        }

        out.buttons    = static_cast<uint16_t>(payload[0])
                       | (static_cast<uint16_t>(payload[1]) << 8);
        out.wheel       = static_cast<int8_t>(payload[2]);
        out.gas_pedal   = static_cast<int8_t>(payload[3]);
        out.brake_pedal = static_cast<int8_t>(payload[4]);
        out.hat0        = (payload[5] >> 4) & 0x0F;
        out.hat1        =  payload[5]       & 0x0F;
        return true;
    }

    case CMD_RESPONSE:
    case CMD_HIGH_LEVEL:
    case CMD_LOW_LEVEL:
        // Response packets to FF queries - we don't need them in this
        // iteration.  Just ignore.
        return false;

    default:
        // Unknown opcode - ignore.
        return false;
    }
}

} // namespace iforce
