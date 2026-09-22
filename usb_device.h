// SPDX-License-Identifier: GPL-2.0-or-later
//
// usb_device.h
//
// Thin RAII wrapper around libusb for opening the Guillemot Force
// Feedback Racing Wheel (06f8:0004) and using its interrupt endpoints.

#pragma once

#include "iforce_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare libusb types to avoid leaking the libusb header into the
// rest of the project.  Only the .cpp needs the actual definitions.
struct libusb_context;
struct libusb_device_handle;

namespace iforce {

struct UsbOpenResult {
    bool ok = false;
    std::string error_message;
};

// Owning wrapper around a libusb context + device handle.
//
// Lifetime:
//   - UsbDevice::open()    -> claim interface 0 of 06f8:0004
//   - UsbDevice::read()    -> blocking interrupt transfer (no async API here)
//   - destructor           -> release interface, close handle, exit context
//
// All transfers (read, write, query) are serialized through an internal
// mutex so that the main-loop thread (which blocks on read()) and the
// ViGEm rumble-callback thread (which calls write()) cannot race against
// libusb's internal event-handling state. Without this serialization,
// concurrent sync transfers on a single libusb context on Windows/WinUSB
// can produce spurious LIBUSB_ERROR_IO returns and silently dropped
// completions.
//
// On any PIPE/IO error the wrapper also calls libusb_clear_halt() on the
// affected endpoint before retrying once. Interrupt endpoints on I-Force
// firmware occasionally NAK/STALL a packet they cannot immediately absorb;
// on WinUSB that halt is sticky and would otherwise wedge every subsequent
// transfer until process exit.
class UsbDevice {
public:
    UsbDevice() = default;
    ~UsbDevice();

    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;
    UsbDevice(UsbDevice&& other) noexcept;
    UsbDevice& operator=(UsbDevice&& other) noexcept;

    // Open device with VID=0x06f8 PID=0x0004, detach the kernel HID driver
    // if needed (libusb_detach_kernel_driver), then claim interface 0.
    UsbOpenResult open(uint16_t vid = USB_VENDOR, uint16_t pid = USB_PRODUCT);

    // Blocking interrupt transfer from EP_IN_ADDR.
    // Returns false on timeout or USB error (check last_error()).
    // `timeout_ms == 0` means "wait forever".
    bool read(std::vector<uint8_t>& buffer, int timeout_ms, int* transferred);

    // Send one already-framed I-Force packet to EP_OUT_ADDR.
    // Default timeout is intentionally short (100 ms): the device normally
    // accepts an interrupt OUT transfer within one USB frame (1 ms). A long
    // timeout blocks the rumble callback thread and risks backing up the
    // ViGEm client. On a real failure the wrapper clears the endpoint
    // halt and retries once before giving up.
    bool write(const std::vector<uint8_t>& packet, int timeout_ms = 100);

    // Read a vendor identification response such as the Linux driver's B/O/M
    // queries. The returned buffer includes the request byte at index 0.
    bool query(uint8_t request, std::vector<uint8_t>& response,
        int timeout_ms = 1000);

    // Clear a sticky STALL on the IN or OUT interrupt endpoint. Safe to call
    // even when the endpoint is not stalled. Returns the libusb status code.
    int clear_halt_in();
    int clear_halt_out();

    void close();

    std::string last_error() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return last_error_;
    }
    bool is_open() const { return handle_ != nullptr; }

private:
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    int iface_ = 0;
    uint8_t ep_in_ = EP_IN_ADDR;
    uint8_t ep_out_ = EP_OUT_ADDR;
    bool detached_ = false; // we detached the kernel driver
    bool claimed_ = false;  // we claimed the interface

    // Serializes all libusb transfers on this handle across threads.
    // See class comment for rationale.
    mutable std::mutex io_mutex_;

    // last_error_ is written under io_mutex_ and must only be read under the
    // same lock. Use last_error_copy() for a thread-safe snapshot.
    std::string last_error_;
};

} // namespace iforce
