# iforce-feedback-wheel-driver-port

User-mode port to modern Windows 10 (probably Windows 11) of the Linux `iforce` input path for the Guillemot/Thrustmaster Force
Feedback Racing Wheel (`VID 0x06f8`, `PID 0x0004`). The current port exposes
the wheel as a virtual Xbox 360 controller through ViGEm and implements a
conservative I-Force force-feedback output path that keeps the wheel
self-centering while driving low-power rumble and impact effects.
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
The output endpoint is `0x02`, which this port uses to upload and drive
I-Force force-feedback effects (see "Force Feedback Behaviour" below).

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

The Linux `btn_wheel[]` table is a guess for this PID and gets several
controls wrong (it has the two paddle shifters reversed and treats the right
hat's up/right directions as generic buttons). The table below reflects the
actual bitmap, measured on the physical wheel by pressing each control and
reading the raw report:

| Bit | Meaning              |
| --- | -------------------- |
| 0   | Right paddle shifter |
| 1   | Left paddle shifter  |
| 2   | Right face button    |
| 3   | Left face button     |
| 4   | Gear up              |
| 5   | Gear down            |
| 6   | Right hat, up        |
| 7   | Right hat, right     |

The high nibble of the hat byte is hat 0, which is presented as the virtual
controller D-pad. The Linux driver maps hat values 0 through 7 to the eight
compass directions; values 8 through 15 are treated as neutral.

The low nibble is hat 1, and on this wheel it carries the *other two*
directions of the right hat: bit 0 is down and bit 1 is left. So the right
hat is split across two bytes -- left/down live in the hat 1 nibble, up/right
live in bits 6/7 of the button byte above -- and no single-nibble decoder
(including the Linux kernel's own second-hat bitmask logic, whose axis signs
are inverted relative to what this wheel reports) can express it. The port
decodes these four bits directly rather than through a shared hat-to-axis
helper; see `right_hat` in `iforce_protocol.h`.

The Linux input ranges are:

- Wheel: `-1920` to `1920`
- Gas: `0` to `255`
- Brake: `0` to `255`
- Hat axes: `-1` to `1`

The port maps the wheel to the virtual pad's left thumb X axis, gas to the
right trigger, and brake to the left trigger. All ten controls (two paddles,
two face buttons, two gear positions, and the four right-hat directions) map
one-to-one onto the ten Xbox 360 digital buttons:

| Control            | Xbox 360 button |
| ------------------ | ---------------- |
| Right paddle        | Y                |
| Left paddle         | X                |
| Right face button   | Start            |
| Left face button    | Back             |
| Gear up             | Right shoulder   |
| Gear down           | Left shoulder    |
| Right hat up        | Left thumb click |
| Right hat right     | B                |
| Right hat down      | Right thumb click|
| Right hat left      | A                |

The gear stick took the shoulder buttons because that is where racing games
put their default shift bindings; the paddles land on X/Y instead. The left
hat (hat 0) drives the D-pad as described above. Unsupported packet types and
truncated packets are ignored.

## What does current implementation

- keeping autocentering enabled at startup with the Linux-compatible payload
  `{ 0x03, strength }` followed by `{ 0x04, 0x01 }`;
- mapping XInput rumble into the full safe I-Force range (`0..0x7F`) instead of
  discarding half of the usable signal;
- uploading explicit per-channel modifier blocks for the periodic channels and
  the impact effect, matching the upstream Linux `iforce-ff.c` structure more
  closely.

## Force Feedback Behaviour

The port now installs a small, Linux-driver-like I-Force effect set for the
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
The payload byte is not a raw 0..100 percentage value; `0x7F` is full-strength
centering. The port currently sends `0x60` (~75% strength) because full
strength fights noticeably with the driven rumble/impact effects.

The port currently installs:

- one short `FF_CONSTANT` impact pulse (roughly 250 ms), driven by the Xbox
  360 rumble values and alternated in polarity to avoid applying a permanent
  steering bias. Its magnitude is scaled so full combined rumble reaches the
  protocol's safe `0x7F` ceiling;
- two continuous `FF_PERIODIC` channels, one for the large motor and one for the
  small motor, using distinct waveforms and periods to feel like a rumble bed
  rather than a single generic buzz.

Every effect core also sets an explicit polar direction (`0x4000`, i.e. 90
degrees / pure +X) in the effect-core packet. This mattered in practice: a
direction of `0x0000` ("north") has no component on a wheel's single steering
axis, so the device accepted and played the effect -- and reported it as
playing in its status packets -- while producing no felt force at all.

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
low speed first. 

## Reference

- Linux USB transport: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-usb.c
- Linux packet decoding: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-packets.c

# AI

Is this done by AI? Yes, it is. I developed my last windows driver 25 years ago, I just wanted my USB wheel to work in windows.
