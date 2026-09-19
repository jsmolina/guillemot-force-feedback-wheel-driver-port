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

## Reference

- Linux USB transport: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-usb.c
- Linux packet decoding: https://github.com/torvalds/linux/blob/master/drivers/input/joystick/iforce/iforce-packets.c
