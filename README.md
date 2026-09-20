# guillemot-force-feedback-wheel-driver-port

User-mode port to modern Windows of the Linux `iforce` input path for the Guillemot/Thrustmaster Force
Feedback Racing Wheel (`VID 0x06f8`, `PID 0x0004`). The current port exposes
the wheel as a virtual Xbox 360 controller through ViGEm. Force-feedback
output is not implemented yet.
<img width="1600" height="1200" alt="image" src="https://github.com/user-attachments/assets/596a1903-14df-4c75-82cd-42b04e7bb9bb" />


## Installing
On Windows, this project is a *user-mode bridge*, not a kernel driver. It reads the physical wheel with libusb and creates a virtual Xbox 360 controller through ViGEmBus.

*Setup*

1. Install the *ViGEmBus* driver on Windows.
2. Connect the wheel.
3. Use Zadig to replace the wheel’s HID driver with *WinUSB* or *libusbK*:
   - Select the Guillemot wheel.
   - Enable “List All Devices”.
   - Install WinUSB/libusbK for the correct interface, usually interface 0.
4. Extract the release ZIP.
5. Run PowerShell or Command Prompt in that folder:

powershell
.\iforce_vigem_port.exe


You should see messages indicating:

text
Wheel 06f8:0004 opened
ViGEm Xbox 360 target online


Press Ctrl+C to stop it.

Verify the virtual controller with:

text
Win+R -> joy.cpl


The wheel should appear as an Xbox 360 controller. Steering maps to left-stick X, gas to the right trigger, brake to the left trigger, and wheel buttons to controller buttons.

Force feedback is currently not implemented. If the program cannot open the wheel, check that Zadig assigned WinUSB/libusbK to the correct interface and that no other application is using the device.

## Device Behaviour

The Linux driver identifies this device as a wheel using the `abs_wheel`
layout. USB input arrives through interface 0 on interrupt endpoint `0x82`.
The output endpoint is `0x02`, although this port does not currently send
force-feedback commands.

Each USB interrupt packet is handled as follows:

```text
byte 0       packet identifier: 0x03 for wheel position data
bytes 1..2   signed 16-bit little-endian wheel position
byte 3       gas value, inverted by the driver: 255 - value
byte 4       brake value, inverted by the driver: 255 - value
byte 5       reserved by the wheel input path
byte 6       button bitmap
byte 7       hat data
```

The wheel packet requires all seven payload bytes after the packet identifier.
The button bitmap uses the following Linux `btn_wheel[]` order:

| Bit | Meaning                 |
| --- | ----------------------- |
| 0   | Gear down / left paddle |
| 1   | Gear up / right paddle  |
| 2   | Wheel button 1          |
| 3   | Wheel button 2          |
| 4   | Wheel button 3          |
| 5   | Wheel button 4          |
| 6   | Wheel button 5          |
| 7   | Wheel button 6          |

The high nibble of the hat byte is hat 0, which is presented as the virtual
controller D-pad. The Linux driver maps hat values 0 through 7 to the eight
compass directions; values 8 through 15 are treated as neutral. The low
nibble contains the secondary hat bits used by other device layouts and is
not used by this wheel mapping.

The Linux input ranges are:

- Wheel: `-1920` to `1920`
- Gas: `0` to `255`
- Brake: `0` to `255`
- Hat axes: `-1` to `1`

The port maps the wheel to the virtual pad's left thumb X axis, gas to the
right trigger, brake to the left trigger, and the eight wheel buttons to
Xbox 360 buttons. Unsupported packet types and truncated packets are
ignored.

## Reference

- Linux USB transport: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-usb.c
- Linux packet decoding: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-packets.c

# AI
Is this done by AI? Yes, it is. I developed my last windows driver 25 years ago, I just wanted my USB wheel to work in windows.
