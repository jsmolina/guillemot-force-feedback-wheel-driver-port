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
// output has a separate implementation. Note: decode_packet logs 0x02 status
// packets to stderr for diagnostics.

#include "iforce_protocol.h"

#include <cstdio>

namespace iforce {

// Hat 0 direction table, copied from the kernel's iforce_hat_to_axis[16].
// Index = high nibble of data[6]. Indices 0..7 are the eight compass
// directions starting at North (so index 0 is *not* neutral); 8..15 are
// zero-initialised in the kernel table and read as centred.
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

HatXY hat1_to_xy(uint8_t hat_bits) {
    // Bitmask, not an index -- see the second-hat block of the kernel's
    // iforce_report_hats_buttons(). The kernel checks bit3 before bit1 and
    // bit0 before bit2, so opposing directions pressed together resolve to
    // the negative axis rather than cancelling.
    HatXY xy{ 0, 0 };
    if (hat_bits & (1u << 3))
        xy.x = -1;
    else if (hat_bits & (1u << 1))
        xy.x = 1;

    if (hat_bits & (1u << 0))
        xy.y = -1;
    else if (hat_bits & (1u << 2))
        xy.y = 1;

    return xy;
}

bool decode_status_report(const uint8_t* data, std::size_t length, StatusReport& out) {
    out = StatusReport{};
    if (data == nullptr || length < 1)
        return false;
    if (data[0] != CMD_STATUS)
        return false;

    const uint8_t* payload = data + 1;
    const std::size_t payload_len = length - 1;
    if (payload_len < 2) {
        out.valid = true;
        return true;
    }

    out.valid = true;
    out.deadman = (payload[0] & 0x02) != 0;
    out.effect_id = static_cast<uint8_t>(payload[1] & 0x7F);
    out.playing = (payload[1] & 0x80) != 0;

    // The device reports one (lo,hi) address per modifier it has just
    // finished absorbing. Same byte layout as Linux
    // iforce-packets.c:mark_core_as_ready, which reads them as
    // get_unaligned_le16(data + j) for j = 3, 5, ...
    for (std::size_t j = 3;
         j + sizeof(uint16_t) <= payload_len;
         j += sizeof(uint16_t)) {
        const uint16_t addr = static_cast<uint16_t>(payload[j])
            | (static_cast<uint16_t>(payload[j + 1]) << 8);
        out.ready_modifier_addresses.push_back(addr);
    }
    return true;
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

    case CMD_STATUS: {
        // Not an input packet; decode_packet returns false so the caller
        // skips the ViGEm update path. But we log it so the user can see
        // the device's acknowledgements of our modifier uploads, which is
        // extremely useful when diagnosing "why does the wheel go silent
        // after a few seconds of rumble".
        StatusReport sr;
        if (decode_status_report(data, length, sr) && sr.valid) {
            std::fprintf(stderr, "iforce status: effect=%u playing=%d deadman=%d",
                static_cast<unsigned>(sr.effect_id),
                static_cast<int>(sr.playing),
                static_cast<int>(sr.deadman));
            if (!sr.ready_modifier_addresses.empty()) {
                std::fprintf(stderr, " ready=");
                for (std::size_t i = 0; i < sr.ready_modifier_addresses.size(); ++i) {
                    if (i) std::fprintf(stderr, ",");
                    std::fprintf(stderr, "%u", static_cast<unsigned>(sr.ready_modifier_addresses[i]));
                }
            }
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
        return false;
    }

    case CMD_JOYSTICK:
    default:
        return false;
    }
}

} // namespace iforce
