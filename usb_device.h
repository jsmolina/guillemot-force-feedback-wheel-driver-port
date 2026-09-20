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
    bool write(const std::vector<uint8_t>& packet, int timeout_ms = 1000);

    // Read a vendor identification response such as the Linux driver's B/O/M
    // queries. The returned buffer includes the request byte at index 0.
    bool query(uint8_t request, std::vector<uint8_t>& response,
        int timeout_ms = 1000);

    void close();

    const std::string& last_error() const { return last_error_; }
    bool is_open() const { return handle_ != nullptr; }

private:
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    int iface_ = 0;
    uint8_t ep_in_ = EP_IN_ADDR;
    uint8_t ep_out_ = EP_OUT_ADDR;
    bool detached_ = false; // we detached the kernel driver
    bool claimed_ = false;  // we claimed the interface
    std::string last_error_;
};

} // namespace iforce
