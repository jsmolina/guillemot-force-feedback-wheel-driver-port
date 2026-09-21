# guillemot-force-feedback-wheel-driver-port

User-mode port to modern Windows of the Linux `iforce` input path for the Guillemot/Thrustmaster Force
Feedback Racing Wheel (`VID 0x06f8`, `PID 0x0004`). The current port exposes
the wheel as a virtual Xbox 360 controller through ViGEm. Force-feedback
output is not implemented yet.
<img width="1600" height="1200" alt="image" src="https://github.com/user-attachments/assets/596a1903-14df-4c75-82cd-42b04e7bb9bb" />

## Installing

On Windows, this project is a _user-mode bridge_, not a kernel driver. It reads the physical wheel with libusb and creates a virtual Xbox 360 controller through ViGEmBus.

_Setup_

1. Install the _ViGEmBus_ driver on Windows.
2. Connect the wheel.
3. Use Zadig to replace the wheel’s HID driver with _WinUSB_ or _libusbK_:
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

Force feedback is implemented in a conservative, Linux-compatible way. The port re-enables the wheel's built-in centering spring at startup and then uploads a small set of I-Force effects for rumble/impacts. If the program cannot open the wheel, check that Zadig assigned WinUSB/libusbK to the correct interface and that no other application is using the device.

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

## Force Feedback Behaviour

The port now installs a small, Linux-compatible I-Force effect set for the
Guillemot wheel. The startup flow is intentionally conservative and mirrors the
Linux driver semantics:

1. Query device readiness (`'O'`) and then effect memory (`'B'`).
2. Re-enable the built-in centering spring with the correct I-Force
   `FF_CMD_AUTOCENTER` payload.
3. Upload a single impact effect and two periodic rumble channels.
4. Enable force feedback and keep the looping periodic effects re-armed so they
   do not silently expire on-device after the 16-bit duration counter elapses.

The centering step is the important compatibility detail: the Linux driver sends
`FF_CMD_AUTOCENTER` as `{ 0x03, magnitude >> 9 }` followed by `{ 0x04, 0x01 }`.
The payload byte is not a raw 0..100 percentage value; for full centering the
encoded strength byte is `0x7F`.

The port currently installs:

- one short `FF_CONSTANT` impact pulse (roughly 250 ms), driven by the Xbox
  360 rumble values and alternated in polarity to avoid applying a permanent
  steering bias;
- two continuous `FF_PERIODIC` channels, one for the large motor and one for the
  small motor, using distinct waveforms and periods to feel like a rumble bed
  rather than a single generic buzz.

The periodic channels intentionally use an explicit envelope modifier instead of
relying on the protocol's `0xFFFF` "no envelope" sentinel, because older I-Force
firmware is known to mishandle that sentinel. This matches the Linux driver's
practical behavior more closely than the idealized protocol description.

The force output is intentionally conservative: XInput rumble values are mapped
into the device's safe `0..0x7F` range and the device's `0x80` byte is avoided,
because the Linux driver notes that `0x80` is a special value that some
firmware revisions mishandle. This keeps the output stable and avoids creating a
bad or inconsistent force profile.

The port still does not attempt to reproduce the richer Immersion/IFC22 effect
model: friction, barriers, springs, custom waveform shaping, and other richer
force effects are not exposed by the Xbox 360 virtual controller API.

If the wheel does not answer the readiness or memory query, the program falls
back to input-only mode and continues without force feedback.

## Build and Test

The virtual Xbox 360 target requires Windows with ViGEmBus installed. The
wheel must be accessible through WinUSB or libusbK (for example, using Zadig)
on interface 0. Build with the vcpkg toolchain and then run the executable:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
.\build\Release\iforce_vigem_port.exe
```

Start a game that produces controller rumble and verify the wheel response at
low speed first. The current macOS development environment cannot perform
this runtime test because the ViGEmClient headers and library are Windows
dependencies, and physical wheel hardware is required for force feedback.

## Reference

- Linux USB transport: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-usb.c
- Linux packet decoding: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-packets.c

# AI

Is this done by AI? Yes, it is. I developed my last windows driver 25 years ago, I just wanted my USB wheel to work in windows.
