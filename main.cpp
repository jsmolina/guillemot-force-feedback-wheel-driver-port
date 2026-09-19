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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "iforce_protocol.h"
#include "usb_device.h"
#include "vigem_gamepad.h"

using namespace iforce;

static std::atomic<bool> g_stop{ false };

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
    log("user-mode port, no FF, buttons + axes only.");
    log("");

    // --- ViGEm setup -----------------------------------------------------
    VigemGamepad pad;
    if (!pad.connect()) {
        log("Failed to create ViGEm Xbox 360 target: %s", pad.last_error().c_str());
        log("Install ViGEmBus driver from https://github.com/ViGEm/ViGEmBus/releases");
        return EXIT_FAILURE;
    }
    log("ViGEm Xbox 360 target online.");

    // --- USB setup -------------------------------------------------------
    UsbDevice dev;
    UsbOpenResult open_rc = dev.open();
    if (!open_rc.ok) {
        log("USB open failed: %s", open_rc.error_message.c_str());
        return EXIT_FAILURE;
    }
    log("Wheel 06f8:0004 opened, interface 0 claimed.");

    // --- Main loop -------------------------------------------------------
    std::vector<uint8_t> buf;
    DeviceState state{};

    log("Reading packets - press Ctrl+C to quit.");
    log("");

    while (!g_stop.load()) {
        int transferred = 0;
        if (!dev.read(buf, /*timeout_ms=*/1000, &transferred)) {
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

        // Re-connection / target-loss detection: ViGEmClient returns an
        // error if the target was removed from the bus.  In that case we
        // try to recreate it.
        if (!pad.update(new_state)) {
            log("ViGEm update failed: %s - reconnecting target...",
                pad.last_error().c_str());
            pad.disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!pad.connect()) {
                log("ViGEm reconnect failed: %s", pad.last_error().c_str());
            }
            continue;
        }

        state = new_state;
    }

    log("");
    log("Shutting down...");
    dev.close();
    pad.disconnect();
    log("Done.");
    return EXIT_SUCCESS;
}
