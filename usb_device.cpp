// SPDX-License-Identifier: GPL-2.0-or-later
//
// usb_device.cpp
//
// Implementation of the libusb wrapper.  All libusb header includes are
// kept here so the rest of the project doesn't depend on libusb at compile
// time (only the link step pulls in -lusb-1.0).

#include "usb_device.h"

#include <libusb-1.0/libusb.h>

#include <utility>

namespace iforce {

UsbDevice::~UsbDevice() {
    close();
}

UsbDevice::UsbDevice(UsbDevice&& other) noexcept
    : ctx_(other.ctx_),
      handle_(other.handle_),
      iface_(other.iface_),
      detached_(other.detached_),
      claimed_(other.claimed_),
      last_error_(std::move(other.last_error_)) {
    other.ctx_ = nullptr;
    other.handle_ = nullptr;
    other.detached_ = false;
    other.claimed_ = false;
}

UsbDevice& UsbDevice::operator=(UsbDevice&& other) noexcept {
    if (this != &other) {
        close();
        ctx_ = other.ctx_;
        handle_ = other.handle_;
        iface_ = other.iface_;
        detached_ = other.detached_;
        claimed_ = other.claimed_;
        last_error_ = std::move(other.last_error_);
        other.ctx_ = nullptr;
        other.handle_ = nullptr;
        other.detached_ = false;
        other.claimed_ = false;
    }
    return *this;
}

UsbOpenResult UsbDevice::open(uint16_t vid, uint16_t pid) {
    UsbOpenResult result;

    close();

    // 1. Initialise a private libusb context (libusb >= 1.0.27 supports
    //    libusb_init_context() for option-based init; older versions fall
    //    back to the deprecated libusb_init()).
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x0100010A)
    const int init_rc = libusb_init_context(&ctx_, /*opts=*/nullptr, /*num_opts=*/0);
#else
    const int init_rc = libusb_init(&ctx_);
#endif
    if (init_rc != LIBUSB_SUCCESS) {
        result.error_message = std::string("libusb_init failed: ")
            + libusb_error_name(init_rc);
        return result;
    }

    // 2. Find the device by VID/PID.  This is a blocking enumeration call.
    handle_ = libusb_open_device_with_vid_pid(ctx_, vid, pid);
    if (handle_ == nullptr) {
        result.error_message = "Could not find/open device 06f8:0004. "
                               "Make sure the wheel is plugged in. "
                               "On Windows you may also need to replace the stock HID driver "
                               "with WinUSB / libusbK via Zadig (interface 0).";
        close();
        return result;
    }

    // 3. Detach the kernel driver (on Windows: the HID stack).  On Linux
    //    this is essential; on Windows with libusb + WinUSB the call is a
    //    no-op (returns LIBUSB_ERROR_NOT_SUPPORTED), which we tolerate.
    const int detach_rc = libusb_detach_kernel_driver(handle_, iface_);
    if (detach_rc == LIBUSB_SUCCESS) {
        detached_ = true;
    } else if (detach_rc != LIBUSB_ERROR_NOT_SUPPORTED
        && detach_rc != LIBUSB_ERROR_NOT_FOUND) {
        // Real error - bail out.
        result.error_message = std::string("libusb_detach_kernel_driver failed: ")
            + libusb_error_name(detach_rc);
        close();
        return result;
    }

    // 4. Claim interface 0.
    const int claim_rc = libusb_claim_interface(handle_, iface_);
    if (claim_rc != LIBUSB_SUCCESS) {
        result.error_message = std::string("libusb_claim_interface failed: ")
            + libusb_error_name(claim_rc);
        close();
        return result;
    }
    claimed_ = true;

    result.ok = true;
    return result;
}

bool UsbDevice::read(std::vector<uint8_t>& buffer, int timeout_ms, int* transferred) {
    if (!is_open()) {
        last_error_ = "UsbDevice::read() called on a closed device";
        return false;
    }
    buffer.resize(EP_MAX_PACKET);
    int actual = 0;
    const int rc = libusb_interrupt_transfer(
        handle_,
        EP_IN_ADDR,
        buffer.data(),
        static_cast<int>(buffer.size()),
        &actual,
        timeout_ms);

    if (transferred)
        *transferred = actual;

    if (rc == LIBUSB_SUCCESS) {
        buffer.resize(static_cast<std::size_t>(actual));
        last_error_.clear();
        return true;
    }

    if (rc == LIBUSB_ERROR_TIMEOUT) {
        last_error_ = "libusb_interrupt_transfer timed out";
    } else {
        last_error_ = std::string("libusb_interrupt_transfer failed: ")
            + libusb_error_name(rc);
    }
    buffer.clear();
    return false;
}

void UsbDevice::close() {
    if (handle_) {
        if (claimed_) {
            libusb_release_interface(handle_, iface_);
            claimed_ = false;
        }
        if (detached_) {
            libusb_attach_kernel_driver(handle_, iface_);
            detached_ = false;
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

} // namespace iforce
