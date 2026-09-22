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

// Build a wheel packet carrying a given button byte and right-hat nibble.
// data[6] packs hat0 in the high nibble and hat1 in the low nibble; the
// captures below all had hat0 idle at 0xF.
static void make_wheel_packet(uint8_t buttons, uint8_t hat1, uint8_t (&out)[8]) {
    out[0] = 0x03;
    out[1] = 0x00; out[2] = 0x00; // wheel
    out[3] = 0x00; out[4] = 0x00; // gas, brake
    out[5] = 0x00;                // unused by the input path
    out[6] = buttons;
    out[7] = static_cast<uint8_t>((0xF << 4) | (hat1 & 0x0F));
}

static void test_right_hat_is_split_across_two_bytes() {
    // These four cases are transcribed from real captures taken by pressing
    // each direction of the right hat on the physical 06f8:0004 wheel.
    //
    //   right hat left   -> buttons 0x00, hat1 0x2
    //   right hat down   -> buttons 0x00, hat1 0x1
    //   right hat up     -> buttons 0x40, hat1 0x0
    //   right hat right  -> buttons 0x80, hat1 0x0
    //
    // So the hat straddles both bytes: left/down are hat1 bits, up/right are
    // the top two bits of the button byte. Any attempt to decode this hat
    // from the hat1 nibble alone loses half of it.
    using namespace right_hat;
    uint8_t packet[8];
    DeviceState s{};

    make_wheel_packet(0x00, 0x2, packet);
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.hat1 & (1u << kLeftBitInHat1));
    assert(!(s.hat1 & (1u << kDownBitInHat1)));
    assert(s.buttons == 0x00);

    make_wheel_packet(0x00, 0x1, packet);
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.hat1 & (1u << kDownBitInHat1));
    assert(!(s.hat1 & (1u << kLeftBitInHat1)));
    assert(s.buttons == 0x00);

    make_wheel_packet(0x40, 0x0, packet);
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.buttons & (1u << kUpBitInButtons));
    assert(s.hat1 == 0x0);

    make_wheel_packet(0x80, 0x0, packet);
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.buttons & (1u << kRightBitInButtons));
    assert(s.hat1 == 0x0);

    // The up/right bits are hat directions, not spare buttons -- nothing else
    // on this wheel drives bit 6 or bit 7.
    static_assert(kUpBitInButtons == 6, "right hat up moved");
    static_assert(kRightBitInButtons == 7, "right hat right moved");
}

static void test_wheel_packet_decode() {
    // id=0x03, then 7 payload bytes.
    // wheel = 0xF000 -> -4096, gas = 255-0x20, brake = 255-0x40,
    // buttons = 0x81, data[6] = 0x21 -> hat0 = 2 (E), hat1 = 0x1 (right hat down)
    const uint8_t packet[] = { 0x03, 0x00, 0xF0, 0x20, 0x40, 0x00, 0x81, 0x21 };
    DeviceState s{};
    assert(decode_packet(packet, sizeof(packet), s));
    assert(s.wheel == -4096);
    assert(s.gas_pedal == 255 - 0x20);
    assert(s.brake_pedal == 255 - 0x40);
    assert(s.buttons == 0x81);
    assert(s.hat0 == 0x2);
    assert(s.hat1 == 0x1);
    assert(hat_to_xy(s.hat0).x == 1); // E

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
    test_right_hat_is_split_across_two_bytes();
    test_wheel_packet_decode();
    test_status_report_decode();
    std::printf("all protocol self-checks passed\n");
    return 0;
}
