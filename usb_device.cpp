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
      ep_in_(other.ep_in_),
      ep_out_(other.ep_out_),
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
        ep_in_ = other.ep_in_;
        ep_out_ = other.ep_out_;
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

    // Use the active descriptors instead of assuming every firmware revision
    // uses the endpoint addresses from the Linux driver.
    libusb_config_descriptor* config = nullptr;
    const int config_rc = libusb_get_active_config_descriptor(
        libusb_get_device(handle_), &config);
    if (config_rc == LIBUSB_SUCCESS && config != nullptr) {
        for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
            const libusb_interface& interface = config->interface[i];
            for (int alt = 0; alt < interface.num_altsetting; ++alt) {
                const libusb_interface_descriptor& descriptor = interface.altsetting[alt];
                if (descriptor.bInterfaceNumber != iface_)
                    continue;
                for (uint8_t ep = 0; ep < descriptor.bNumEndpoints; ++ep) {
                    const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[ep];
                    if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                        != LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                        continue;
                    }
                    if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
                        == LIBUSB_ENDPOINT_IN) {
                        ep_in_ = endpoint.bEndpointAddress;
                    } else {
                        ep_out_ = endpoint.bEndpointAddress;
                    }
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    result.ok = true;
    return result;
}

bool UsbDevice::read(std::vector<uint8_t>& buffer, int timeout_ms, int* transferred) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        last_error_ = "UsbDevice::read() called on a closed device";
        return false;
    }
    buffer.resize(EP_MAX_PACKET);
    int actual = 0;
    int rc = libusb_interrupt_transfer(
        handle_,
        ep_in_,
        buffer.data(),
        static_cast<int>(buffer.size()),
        &actual,
        timeout_ms);

    // I-Force firmware occasionally NAKs an interrupt IN URB when it is busy
    // absorbing an effect-upload burst. On WinUSB a sustained NAK can flip the
    // endpoint into a sticky STALL state which would otherwise wedge every
    // subsequent read until process exit. Clear the halt and retry once.
    if (rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_IO) {
        libusb_clear_halt(handle_, ep_in_);
        actual = 0;
        rc = libusb_interrupt_transfer(
            handle_,
            ep_in_,
            buffer.data(),
            static_cast<int>(buffer.size()),
            &actual,
            timeout_ms);
    }

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

bool UsbDevice::write(const std::vector<uint8_t>& packet, int timeout_ms) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        last_error_ = "UsbDevice::write() called on a closed device";
        return false;
    }
    if (packet.empty() || packet.size() > EP_MAX_PACKET) {
        last_error_ = "UsbDevice::write() packet length is outside endpoint limits";
        return false;
    }

    int transferred = 0;
    int rc = libusb_interrupt_transfer(
        handle_,
        ep_out_,
        const_cast<unsigned char*>(packet.data()),
        static_cast<int>(packet.size()),
        &transferred,
        timeout_ms);

    // The single most common cause of "USB IO error after many presses" is a
    // STALL on the OUT endpoint that the host never clears. On WinUSB the
    // halt is sticky: every subsequent write returns LIBUSB_ERROR_PIPE forever.
    // Clear the halt and retry the transfer once before giving up.
    if (rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_IO) {
        libusb_clear_halt(handle_, ep_out_);
        transferred = 0;
        rc = libusb_interrupt_transfer(
            handle_,
            ep_out_,
            const_cast<unsigned char*>(packet.data()),
            static_cast<int>(packet.size()),
            &transferred,
            timeout_ms);
    }

    if (rc != LIBUSB_SUCCESS) {
        last_error_ = std::string("libusb_interrupt_transfer (OUT) failed: ")
            + libusb_error_name(rc);
        return false;
    }
    if (transferred != static_cast<int>(packet.size())) {
        last_error_ = "libusb interrupt OUT transfer was short";
        return false;
    }

    last_error_.clear();
    return true;
}

bool UsbDevice::query(uint8_t request, std::vector<uint8_t>& response,
    int timeout_ms) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        last_error_ = "UsbDevice::query() called on a closed device";
        return false;
    }

    response.assign(EP_MAX_PACKET, 0);
    int transferred = libusb_control_transfer(
        handle_,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR
            | LIBUSB_RECIPIENT_INTERFACE,
        request,
        0,
        static_cast<uint16_t>(iface_),
        response.data(),
        static_cast<uint16_t>(response.size()),
        timeout_ms);

    // Control transfers can also stall (e.g. unsupported query like 'M' on
    // some firmware revisions). Clear the stall on the control endpoint 0
    // so the next query can succeed.
    if (transferred == LIBUSB_ERROR_PIPE || transferred == LIBUSB_ERROR_IO) {
        libusb_clear_halt(handle_, 0);
        transferred = libusb_control_transfer(
            handle_,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR
                | LIBUSB_RECIPIENT_INTERFACE,
            request,
            0,
            static_cast<uint16_t>(iface_),
            response.data(),
            static_cast<uint16_t>(response.size()),
            timeout_ms);
    }

    if (transferred < 0) {
        last_error_ = std::string("libusb_control_transfer failed: ")
            + libusb_error_name(transferred);
        response.clear();
        return false;
    }
    response.resize(static_cast<std::size_t>(transferred));
    if (response.empty() || response[0] != request) {
        last_error_ = "I-Force query returned an unexpected response";
        response.clear();
        return false;
    }

    last_error_.clear();
    return true;
}

int UsbDevice::clear_halt_in() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open())
        return LIBUSB_ERROR_NO_DEVICE;
    return libusb_clear_halt(handle_, ep_in_);
}

int UsbDevice::clear_halt_out() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open())
        return LIBUSB_ERROR_NO_DEVICE;
    return libusb_clear_halt(handle_, ep_out_);
}

void UsbDevice::close() {
    std::lock_guard<std::mutex> lock(io_mutex_);
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
    ep_in_ = EP_IN_ADDR;
    ep_out_ = EP_OUT_ADDR;
}

} // namespace iforce
