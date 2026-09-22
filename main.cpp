// SPDX-License-Identifier: GPL-2.0-or-later
//
// main.cpp
//
// Glue: open the Guillemot wheel via libusb, push every interrupt-IN
// packet through the iforce protocol decoder, and forward the decoded
// state to a ViGEm Xbox 360 virtual gamepad.
//
// Ctrl-C cleanly tears everything down.

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "iforce_force_feedback.h"
#include "iforce_protocol.h"
#include "usb_device.h"
#include "vigem_gamepad.h"

using namespace iforce;

static std::atomic<bool> g_stop{ false };
static std::atomic<float> g_rumble_scale{ 1.0f };

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT
        || ctrl == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void on_sigint(int) {
    g_stop.store(true);
}
#endif

static void log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int main() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);
#endif

    log("iforce-vigem-port: Guillemot Force Feedback Racing Wheel (06f8:0004)");
    log("user-mode port, buttons + axes with experimental force feedback.");
    log("");

    // --- USB setup -------------------------------------------------------
    UsbDevice dev;
    UsbOpenResult open_rc = dev.open();
    if (!open_rc.ok) {
        log("USB open failed: %s", open_rc.error_message.c_str());
        return EXIT_FAILURE;
    }
    log("Wheel 06f8:0004 opened, interface 0 claimed.");

    IForceFeedback force_feedback(dev);
    if (force_feedback.initialize()) {
        log("I-Force feedback online: low-strength damper enabled.");
    } else {
        log("I-Force feedback unavailable: %s", force_feedback.last_error().c_str());
        log("Continuing with input only.");
    }

    // --- ViGEm setup -----------------------------------------------------
    VigemGamepad pad;
    VigemGamepad::RumbleCallback rumble_callback;
    if (force_feedback.is_enabled()) {
        rumble_callback = [&force_feedback](uint8_t large_motor,
                              uint8_t small_motor) {
            const float scale = g_rumble_scale.load();
            const uint8_t adjusted_large = static_cast<uint8_t>(std::clamp(
                static_cast<float>(large_motor) * scale, 0.0f, 255.0f));
            const uint8_t adjusted_small = static_cast<uint8_t>(std::clamp(
                static_cast<float>(small_motor) * scale, 0.0f, 255.0f));
            force_feedback.on_rumble(adjusted_large, adjusted_small);
        };
    }
    if (!pad.connect(rumble_callback)) {
        log("Failed to create ViGEm Xbox 360 target: %s", pad.last_error().c_str());
        log("Install ViGEmBus driver from https://github.com/ViGEm/ViGEmBus/releases");
        force_feedback.shutdown();
        return EXIT_FAILURE;
    }
    // connect() can succeed (buttons/axes work) while still failing to wire
    // up the rumble notification -- that failure is deliberately non-fatal,
    // so it only shows up in last_error() rather than the return value.
    // Check for it here or force-feedback can look correct end-to-end and
    // still never receive a single rumble callback.
    if (!pad.last_error().empty()) {
        log("warning: %s", pad.last_error().c_str());
    }
    log("ViGEm Xbox 360 target online.");

    // --- Main loop -------------------------------------------------------
    std::vector<uint8_t> buf;
    DeviceState state{};

    log("Reading packets - press Ctrl+C to quit.");
    log("");

    while (!g_stop.load()) {
        // Re-arm the two looping periodic channels before their 16-bit
        // effect-duration counter elapses (~65.5s). tick() is internally
        // rate-limited (kEffectRearmInterval=30s), so calling it every
        // iteration is cheap. Without this, the rumble bed silently dies
        // after one minute of uptime even though initialize() succeeded.
        force_feedback.tick();

#ifdef _WIN32
        static bool key_a_prev = false;
        static bool key_plus_prev = false;
        static bool key_minus_prev = false;

        const bool key_a_now = (GetAsyncKeyState('A') & 0x8000) != 0;
        if (key_a_now != key_a_prev && force_feedback.is_enabled()) {
            if (key_a_now) {
                force_feedback.on_rumble(255, 255);
                log("debug: test impact pulse triggered");
                if (!force_feedback.last_error().empty()) {
                    log("debug: on_rumble reported: %s",
                        force_feedback.last_error().c_str());
                }
            } else {
                // Release: without this, on_rumble()'s edge-detect and
                // magnitude dedup both latch at "255" forever after the
                // first press, so a second press is a silent no-op and
                // the wheel is left rumbling at full strength indefinitely.
                force_feedback.on_rumble(0, 0);
            }
        }
        key_a_prev = key_a_now;

        const bool key_plus_now = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
        if (key_plus_now && !key_plus_prev) {
            float next = g_rumble_scale.load() + 0.1f;
            g_rumble_scale.store(std::clamp(next, 0.1f, 2.0f));
            log("ramp strength: %.2f", g_rumble_scale.load());
        }
        key_plus_prev = key_plus_now;

        const bool key_minus_now = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
        if (key_minus_now && !key_minus_prev) {
            float next = g_rumble_scale.load() - 0.1f;
            g_rumble_scale.store(std::clamp(next, 0.1f, 2.0f));
            log("ramp strength: %.2f", g_rumble_scale.load());
        }
        key_minus_prev = key_minus_now;
#endif

        int transferred = 0;
        if (!dev.read(buf, /*timeout_ms=*/100, &transferred)) {
            if (g_stop.load())
                break;
            // Timeouts are expected when the wheel is idle; only log real
            // transfer errors.
            if (dev.last_error().find("timed out") == std::string::npos) {
                log("USB read error: %s", dev.last_error().c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Decode + forward.
        DeviceState new_state{};
        if (!decode_packet(buf.data(), buf.size(), new_state)) {
            continue; // not an input packet, ignore
        }

        if (new_state.buttons != state.buttons
            || new_state.hat0 != state.hat0
            || new_state.hat1 != state.hat1) {
            log("buttons: 0x%02X  bits=%d%d%d%d%d%d%d%d (bit7..bit0)  hat0=0x%X hat1=0x%X",
                new_state.buttons,
                (new_state.buttons >> 7) & 1, (new_state.buttons >> 6) & 1,
                (new_state.buttons >> 5) & 1, (new_state.buttons >> 4) & 1,
                (new_state.buttons >> 3) & 1, (new_state.buttons >> 2) & 1,
                (new_state.buttons >> 1) & 1, (new_state.buttons >> 0) & 1,
                new_state.hat0, new_state.hat1);
        }

        // Re-connection / target-loss detection: ViGEmClient returns an
        // error if the target was removed from the bus.  In that case we
        // try to recreate it.
        if (!pad.update(new_state)) {
            log("ViGEm update failed: %s - reconnecting target...",
                pad.last_error().c_str());
            pad.disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!pad.connect(rumble_callback)) {
                log("ViGEm reconnect failed: %s", pad.last_error().c_str());
            }
            continue;
        }

        state = new_state;
    }

    log("");
    log("Shutting down...");
    // Disable force feedback FIRST: on_rumble() checks `enabled_` and
    // returns immediately once shutdown() clears it. If a rumble
    // notification is mid-flight on ViGEm's worker thread while we tear
    // down, this ensures it safely no-ops instead of racing pad.disconnect()
    // tearing down the target it's about to be called through.
    force_feedback.shutdown();
    pad.disconnect();
    dev.close();
    log("Done.");
    return EXIT_SUCCESS;
}
