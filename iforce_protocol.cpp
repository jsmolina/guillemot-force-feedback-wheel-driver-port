// SPDX-License-Identifier: GPL-2.0-or-later
//
// iforce_protocol.cpp
//
// Implementation of the iforce USB packet decoder.
//
// The decoder is a 1-to-1 port of the kernel logic found in:
//   - drivers/input/joystick/iforce/iforce-usb.c  (USB IRQ path)
//   - drivers/input/joystick/iforce/iforce-packets.c (packet dispatch)
//
// The read path remains a direct port of the kernel decoder. Force-feedback
// output has a separate implementation so this decoder stays side-effect free.

#include "iforce_protocol.h"

namespace iforce {

// Hat table (same one used by the kernel `iforce_hat_to_xy` helper).
// Index = hat nibble (0..15).  Only indices 0..8 are valid directions,
// 0 means "neutral / centred".  Indices 9..15 fall through to neutral.
static const HatXY kHatTable[16] = {
    /* 0 */ { 0, -1 },  // N
    /* 1 */ { 1, -1 },  // NE
    /* 2 */ { 1, 0 },   // E
    /* 3 */ { 1, 1 },   // SE
    /* 4 */ { 0, 1 },   // S
    /* 5 */ { -1, 1 },  // SW
    /* 6 */ { -1, 0 },  // W
    /* 7 */ { -1, -1 }, // NW
    /* 8..15 -> neutral fallback */
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }
};

HatXY hat_to_xy(uint8_t hat_code) {
    return kHatTable[hat_code & 0x0F];
}

bool decode_packet(const uint8_t* data, std::size_t length, DeviceState& out) {
    if (data == nullptr || length < 1) {
        return false;
    }

    const uint8_t packet_id = data[0];
    const uint8_t* payload = data + 1;
    const std::size_t payload_len = length - 1;

    switch (packet_id) {
    case CMD_WHEEL: {
        if (payload_len < 7) {
            return false;
        }

        int wheel = static_cast<int>(payload[0])
            | (static_cast<int>(payload[1]) << 8);
        if (wheel >= 0x8000)
            wheel -= 0x10000;
        out.wheel = static_cast<int16_t>(wheel);
        out.gas_pedal = static_cast<uint8_t>(255u - payload[2]);
        out.brake_pedal = static_cast<uint8_t>(255u - payload[3]);
        out.buttons = payload[5];
        out.hat0 = (payload[6] >> 4) & 0x0F;
        out.hat1 = payload[6] & 0x0F;
        return true;
    }

    case CMD_JOYSTICK:
    case CMD_STATUS:
    default:
        return false;
    }
}

} // namespace iforce
