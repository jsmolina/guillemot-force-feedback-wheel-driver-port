# guillemot-force-feedback-wheel-driver-port

User-mode port of the Linux `iforce` input path for the Guillemot Force
Feedback Racing Wheel (`VID 0x06f8`, `PID 0x0004`). The current port exposes
the wheel as a virtual Xbox 360 controller through ViGEm. Force-feedback
output is not implemented yet.

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

## Experimental Force Feedback

The port now includes a conservative I-Force output path for the Guillemot
wheel. At startup it queries the device's effect memory and enables two
effects only when the device reports enough memory:

- A low-strength `Damper` condition effect (`25%`) that remains active while
  the virtual controller is connected.
- A short `Constant Force` impact effect (`250 ms`) driven by the Xbox 360
  `LargeMotor` and `SmallMotor` rumble values.

XInput does not provide force direction or Immersion effect type, so impact
polarity alternates to avoid applying a permanent steering bias. This is an
approximation for collisions and vibration, not a faithful translation of
`IFC22.dll` effects. `Friction`, springs, barriers, waveforms, and custom
effects are not exposed by the virtual Xbox 360 interface.

The implementation follows the Linux `iforce` packet lifecycle: it queries
device readiness and memory, disables the built-in autocenter, uploads effect
modifiers, enables force feedback, and stops all effects during shutdown.
Keep the first physical test at low speed and be ready to disconnect the
wheel. If the device does not answer the readiness or memory query, the
program continues in input-only mode.

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
