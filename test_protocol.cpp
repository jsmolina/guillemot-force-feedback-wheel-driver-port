// SPDX-License-Identifier: GPL-2.0-or-later
//
// test_protocol.cpp
//
// Self-check for the pure decode logic in iforce_protocol.cpp. Builds and
// runs on any host (no libusb, no ViGEm, no hardware).
//
//   c++ -std=c++17 test_protocol.cpp iforce_protocol.cpp -o test_protocol && ./test_protocol

#include "iforce_protocol.h"

#include <cassert>
#include <cstdio>

using namespace iforce;

static void test_hat0_is_a_direction_index() {
    // Index into the kernel's iforce_hat_to_axis[]. Index 0 is North, NOT
    // neutral -- regressing this to a "0 means centred" table would break
    // the up direction.
    assert(hat_to_xy(0).x == 0 && hat_to_xy(0).y == -1);  // N
    assert(hat_to_xy(2).x == 1 && hat_to_xy(2).y == 0);   // E
    assert(hat_to_xy(4).x == 0 && hat_to_xy(4).y == 1);   // S
    assert(hat_to_xy(6).x == -1 && hat_to_xy(6).y == 0);  // W
    assert(hat_to_xy(7).x == -1 && hat_to_xy(7).y == -1); // NW
    // 8..15 are zero-initialised in the kernel table -> centred.
    assert(hat_to_xy(8).x == 0 && hat_to_xy(8).y == 0);
    assert(hat_to_xy(15).x == 0 && hat_to_xy(15).y == 0);
}

static void test_hat1_is_a_bitmask_not_an_index() {
    // The whole point of hat1_to_xy: these are bit positions, so the results
    // must NOT match hat_to_xy() for the same numeric value.
    assert(hat1_to_xy(0).x == 0 && hat1_to_xy(0).y == 0);     // nothing held
    assert(hat1_to_xy(1u << 3).x == -1);                      // bit3 -> X=-1
    assert(hat1_to_xy(1u << 1).x == 1);                       // bit1 -> X=+1
    assert(hat1_to_xy(1u << 0).y == -1);                      // bit0 -> Y=-1
    assert(hat1_to_xy(1u << 2).y == 1);                       // bit2 -> Y=+1

    // Diagonal: bit1 (X=+1) | bit2 (Y=+1) -> SE
    assert(hat1_to_xy((1u << 1) | (1u << 2)).x == 1);
    assert(hat1_to_xy((1u << 1) | (1u << 2)).y == 1);

    // Opposing directions: kernel tests bit3 before bit1, bit0 before bit2,
    // so the negative axis wins instead of cancelling to 0.
    assert(hat1_to_xy((1u << 3) | (1u << 1)).x == -1);
    assert(hat1_to_xy((1u << 0) | (1u << 2)).y == -1);

    // Guard against anyone "simplifying" hat1 back into a table lookup:
    // value 1 means Y=-1 as a bitmask, but NE (x=1,y=-1) as an index.
    assert(hat1_to_xy(1).x == 0);
    assert(hat_to_xy(1).x == 1);
}

static void test_wheel_packet_decode() {
    // id=0x03, then 7 payload bytes.
    // wheel = 0xF000 -> -4096, gas = 255-0x20, brake = 255-0x40,
    // buttons = 0x81, data[6] = 0x21 -> hat0 = 2 (E), hat1 = 0x1 (Y=-1)
    const uint8_t packet[] = { 0x03, 0x00, 0xF0, 0x20, 0x40, 0x00, 0x81, 0x21 };
    DeviceState s{};
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.wheel == -4096);
    assert(s.gas_pedal == 255 - 0x20);
    assert(s.brake_pedal == 255 - 0x40);
    assert(s.buttons == 0x81);
    assert(s.hat0 == 0x2);
    assert(s.hat1 == 0x1);
    assert(hat_to_xy(s.hat0).x == 1);   // E
    assert(hat1_to_xy(s.hat1).y == -1); // bit0

    // Truncated packet (kernel: `if (len < 7) break;`) must be rejected.
    DeviceState ignored{};
    assert(!decode_packet(packet, 7, ignored));
}

static void test_status_report_decode() {
    // id=0x02, payload[0]=0x02 (deadman), payload[1]=0x81 (effect 1, playing),
    // payload[2] filler, then LE16 modifier addresses 16 and 42.
    const uint8_t packet[] = { 0x02, 0x02, 0x81, 0x00, 0x10, 0x00, 0x2A, 0x00 };
    StatusReport sr;
    assert(decode_status_report(packet, sizeof(packet), sr));
    assert(sr.valid);
    assert(sr.deadman);
    assert(sr.effect_id == 1);
    assert(sr.playing);
    assert(sr.ready_modifier_addresses.size() == 2);
    assert(sr.ready_modifier_addresses[0] == 16);
    assert(sr.ready_modifier_addresses[1] == 42);

    // Status reports are not input packets -> decode_packet returns false.
    DeviceState s{};
    assert(!decode_packet(packet, sizeof(packet), s));
}

int main() {
    test_hat0_is_a_direction_index();
    test_hat1_is_a_bitmask_not_an_index();
    test_wheel_packet_decode();
    test_status_report_decode();
    std::printf("all protocol self-checks passed\n");
    return 0;
}
