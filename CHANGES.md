# Changes applied to fix "USB IO error after many A presses" and "no vibration"

## Root cause summary

The Windows port re-implements the Linux kernel `iforce` driver as a
user-mode libusb + ViGEmBus bridge. Two architectural mismatches with
the Linux driver caused the symptoms:

1. **The Linux driver uses async URBs + a circular xmit buffer** and
   only ever has one OUT URB in flight at a time. The device emits a
   `0x02` status report after it has absorbed each modifier upload,
   and the kernel gates further uploads on that acknowledgement via the
   `FF_CORE_UPDATE` bit. The Windows port uses blocking
   `libusb_interrupt_transfer` for every `send_command` call, with no
   flow control, no status-report handling, and **no endpoint-stall
   recovery**. On WinUSB a single STALL on the OUT endpoint is sticky:
   every subsequent write returns `LIBUSB_ERROR_PIPE` forever.

2. **`tick()` is never called from `main.cpp`**, so the two looping
   periodic channels (installed with `duration = 0xFFFF`, the 16-bit
   max = 65.5 s) silently expire on the device after one minute of
   uptime. Pressing A still fires the one-shot impact pulse, but the
   continuous rumble bed is dead — which is why "vibration never fires"
   if the program has been running for any non-trivial amount of time
   before the user starts testing.

Below are the concrete fixes.

## 1. `usb_device.h` / `usb_device.cpp` — USB transport layer

### BUG A — `libusb_clear_halt()` after a stalled OUT/IN transfer

The original `UsbDevice::write` returned `false` on
`LIBUSB_ERROR_PIPE`/`LIBUSB_ERROR_IO` without clearing the endpoint
halt. On WinUSB that halt is sticky and every subsequent transfer
fails with the same code, producing exactly the symptom the user
reported: "USB IO error after many A presses".

Both `read()`, `write()`, and `query()` now call
`libusb_clear_halt(handle_, ep)` on `LIBUSB_ERROR_PIPE` or
`LIBUSB_ERROR_IO` and retry the transfer once before giving up. The
new public helpers `clear_halt_in()` / `clear_halt_out()` are also
exposed so callers can proactively clear the endpoint if they detect
a failure upstream.

### BUG B — Concurrent sync libusb transfers across threads

The main thread blocks inside `libusb_interrupt_transfer` on `EP_IN`
for up to one second per loop iteration. The ViGEm rumble callback
runs on a ViGEmBus worker thread and calls `device_.write()` which
issues a concurrent `libusb_interrupt_transfer` on `EP_OUT`.
libusb's synchronous API is documented as thread-safe, but on Windows
+ WinUSB, two threads driving the event loop simultaneously can lose
each other's OVERLAPPED completions, surfacing as spurious
`LIBUSB_ERROR_IO`.

`UsbDevice` now owns a `std::mutex io_mutex_` that is acquired by
`read()`, `write()`, and `query()` for the duration of every
`libusb_*_transfer` call. The move constructor / move assignment
operator are unaffected (the mutex is non-movable, so the moved-to
object default-constructs a fresh one — which is exactly what we
want).

### CONCERN O — Write timeout lowered from 1000 ms to 100 ms

The default `write` timeout was 1000 ms. A single `on_rumble` call
that issues two PERIOD updates + MAGNITUDE + PLAY can therefore
block the ViGEm worker thread for up to four seconds if the device is
NAKing. That backs up ViGEm's internal callback queue and can stall
the whole virtual-controller pipeline. The new default is 100 ms,
which is plenty for one USB frame (1 ms) plus margin.

## 2. `iforce_force_feedback.h` / `.cpp` — flow control + diagnostics

### BUG C — Throttle periodic magnitude updates

`on_rumble` previously sent a PERIOD packet on every rumble callback
where the magnitude had changed. The rumble callback fires at tens of
Hz from ViGEm, and each PERIOD packet rewrites the same modifier
block on the device. Some I-Force firmware revisions NAK the second
update if the first hasn't been absorbed — and on WinUSB a NAK-storm
flips into a STALL, which then triggers BUG A and wedges the pipe
permanently.

The Linux kernel gates this with `FF_CORE_UPDATE` and the device's
`0x02` status report. The Windows port now throttles on the host
side: at most one PERIOD write per channel every 20 ms (the device
takes ~16 ms = one USB frame to absorb a PERIOD update). The
per-channel "last send" timestamps are stored in
`last_large_period_send_` / `last_small_period_send_`.

### CONCERN E — Surface write failures to the log

`on_rumble` previously swallowed `device_.write()` failures silently
— the only USB error that surfaced in the log was the downstream
read-side error in `main.cpp`. The new code:

- Counts consecutive write failures in `consecutive_write_failures_`.
- Logs the first failure and every 20th failure thereafter (so a
  permanently stalled pipe does not spam the log).
- On the impact-trigger path, both the MAGNITUDE update and the
  PLAY command log their failure reasons separately.
- On a failed periodic update, the code intentionally does NOT
  update `last_large_magnitude_` so the next `on_rumble` call
  retries the same value, which is what we want until the endpoint
  recovers.

## 3. `iforce_protocol.h` / `.cpp` — status reports

The kernel's `iforce_process_packet` handles `0x02` status reports
(`iforce-packets.c:189-215`) by calling `mark_core_as_ready` for
every modifier address listed in the payload. The Windows port
dropped them in the `default:` case of `decode_packet`.

A new `StatusReport` struct + `decode_status_report` function parse
the same fields the kernel reads:
- `payload[0] & 0x02` — BTN_DEAD state
- `payload[1] & 0x7F` — effect id
- `payload[1] & 0x80` — currently-playing flag
- `payload[3..]` — little-endian u16 modifier addresses that the
  device has just absorbed

`decode_packet` now logs every `0x02` packet to stderr so the user
can see the device's acknowledgements during diagnosis. The function
still returns `false` for `0x02` so the existing ViGEm update path
is unaffected.

## 4. `main.cpp` — call `tick()` and lower the read timeout

### BUG D — `tick()` was dead code

`IForceFeedback::tick()` is defined to re-arm the two looping
periodic channels before their 16-bit `duration` counter elapses
(~65.5 s), but `main.cpp` never called it. So after one minute of
uptime, both periodic channels silently expired on the device and
pressing A appeared to "do nothing" even though `initialize()`
reported success.

The main loop now calls `force_feedback.tick()` once per iteration.
The function is internally rate-limited to `kEffectRearmInterval =
30 s`, so calling it every loop is cheap.

### Read timeout lowered from 1000 ms to 100 ms

`dev.read(buf, /*timeout_ms=*/1000, ...)` meant the main loop spun
at most once per second when the wheel was idle. That is too coarse
to reliably call `tick()` before the 65.5 s periodic-effect duration
lapses. The new `kReadTimeoutMs = 100` makes the loop ten times more
responsive and ensures `tick()` is called at least every 100 ms.

### Surface FF-layer errors alongside USB read errors

The "USB IO error" the user was seeing in the log came from the
downstream `dev.read()` failure, not the upstream `device_.write()`
failure that was actually wedging the pipe. The new error path now
also prints `force_feedback.last_error()` so the root cause is
visible.

When the A debug key is pressed, the log line now distinguishes
"debug: test impact pulse triggered" (success path) from "debug A:
<error message>" (failure path) so the user can immediately see if
on_rumble hit a stall.

## Byte-for-byte protocol compatibility

A separate analysis (see the conversation thread) compared every
packet the Windows port sends against the bytes the Linux driver
puts on the wire:

- `FF_CMD_MAGNITUDE` 0x0303 — identical
- `FF_CMD_PERIOD` 0x0407 — identical
- `FF_CMD_ENVELOPE` 0x0208 — identical (for the zero-level values
  used by the port)
- `FF_CMD_EFFECT` 0x010E — identical (for the zero direction /
  interval / delay values used by the port)
- `FF_CMD_PLAY` 0x4103 — identical for value ∈ {0, 1}
- `FF_CMD_AUTOCENTER` 0x4002 — identical byte layout (the value
  differs: the port intentionally enables a full-strength
  centering spring at startup, the kernel disables it)
- `FF_CMD_ENABLE` 0x4201 — identical

**No protocol-layer bug was found.** All the failure modes are in
the transport layer (USB I/O) and the effect-lifecycle management
(`tick()`).

## Files modified

- `usb_device.h` — add `io_mutex_`, `clear_halt_in/out()`, lower
  default `write` timeout to 100 ms.
- `usb_device.cpp` — lock `io_mutex_` in `read`/`write`/`query`;
  call `libusb_clear_halt` + retry once on `LIBUSB_ERROR_PIPE` /
  `LIBUSB_ERROR_IO`; implement `clear_halt_in/out`.
- `iforce_force_feedback.h` — add `kMinPeriodicUpdateInterval`,
  per-channel "last send" timestamps, `consecutive_write_failures_`.
- `iforce_force_feedback.cpp` — throttle periodic magnitude updates
  to one per 20 ms per channel; log write failures (first + every
  20th); log impact MAGNITUDE / PLAY failures individually; on a
  failed periodic update, do NOT advance `last_*_magnitude_` so
  the next call retries the same value.
- `iforce_protocol.h` — add `StatusReport` struct +
  `decode_status_report`; declare `<vector>` include.
- `iforce_protocol.cpp` — implement `decode_status_report`;
  `decode_packet` now logs `0x02` status reports to stderr instead
  of silently dropping them.
- `main.cpp` — call `force_feedback.tick()` once per main-loop
  iteration; lower `dev.read` timeout from 1000 ms to 100 ms;
  print `force_feedback.last_error()` alongside the read-side
  USB error; print a distinct "debug A: <error>" line when
  `on_rumble` fails.

## 5. Post-review fixes (2026-09-22)

Four issues found during code review after the initial patch set were fixed:

### FIX F1 — `close()` not serialized

`UsbDevice::close()` touched `handle_`, `claimed_`, and `detached_` with no
lock. A ViGEm rumble callback already mid-flight in `write()` (holding
`io_mutex_`) could race teardown. `close()` now acquires `io_mutex_` at entry.

### FIX F2 — `last_error()` data race + misleading comment

The previous comment said "worst case is a torn read of an SSO pointer" to
justify a lock-free read of `std::string` — that is undefined behavior, not a
bounded worst case. `last_error()` is now a value-returning method that locks
`io_mutex_` before copying the string. `io_mutex_` is now `mutable` so it can
be locked in the `const` getter.

### FIX F3 — Stale "side-effect free" comment in `iforce_protocol.cpp`

The file header said "this decoder stays side-effect free" but
`decode_packet` logs to `stderr` for every `0x02` status packet. Updated the
comment to say the function logs status packets for diagnostics.

### FIX F4 — Read timeout still 1000 ms despite CHANGES.md §4

`main.cpp` had `dev.read(buf, /*timeout_ms=*/1000, ...)` even though §4 above
said it was lowered to 100 ms. Corrected to 100 ms so the `tick()` call at the
top of the loop fires at least every 100 ms, consistent with the stated
rationale.

## 6. Protocol audit against `iforce_linux_reference/` (2026-09-22)

Every outgoing packet was compared byte-for-byte against the kernel source.
**All command encodings match**: `MAGNITUDE` 0x0303, `PERIOD` 0x0407,
`ENVELOPE` 0x0208, `EFFECT` 0x010E (all 14 bytes), `AUTOCENTER` 0x4002
(`0xFFFF >> 9 == 0x7F`, matching `iforce_set_autocenter`), `ENABLE` 0x4201
(`"\004"`/`"\001"`). `HIFIX80` -> `signed_level_byte`, the wire framing
(`[HI(cmd)][payload]` with length `LO(cmd)`), the modifier-chunk sizes
(2/12/14 bytes, 2-byte aligned) and `decode_status_report` are all faithful.

Three behavioural bugs were found and fixed.

### BUG G — Rumble magnitude was clipped, not scaled

`motor_to_magnitude_byte` was `std::min<uint16_t>(motor, 0x7F)`. That clamps
rather than scales, so every XInput motor value from 128 to 255 produced the
identical force: the entire top half of the rumble range was dead, and a game
ramping rumble from 50% to 100% felt like nothing changed. The old comment
claimed this "uses the full safe scale instead of cutting the range in half",
but clipping is strictly worse than halving, which at least stays monotonic.

Now `(motor * 0x7F) / 0xFF`, which maps 0..255 linearly onto the 0..0x7F
range the protocol considers safe (`iforce.h` warns 0x80 is special).

### BUG H — `tick()` reimplemented a workaround the protocol already has

`iforce_control_playback()` (iforce-packets.c:82) encodes a **repeat count**:

```c
data[1] = (value > 0) ? ((value > 1) ? 0x41 : 0x01) : 0;
data[2] = LO(value);
```

The port always sent `{id, 0x01, 0x01}` — play *once*. Combined with the
16-bit `duration` field maxing out at 0xFFFF ms (65.5 s), that is exactly why
the periodic channels lapsed and why `tick()` had to exist.

New `play_effect(effect_id, repeat_count)` helper mirrors the kernel encoding,
and the two looping channels now start with `kPeriodicRepeatCount = 0xFF`
(255 x 65.5 s, about 4.6 hours). This also removes a real artifact: re-issuing
PLAY every 30 s restarted the waveform from phase 0.

`tick()` is **kept as a safety net** but is now expected to be a no-op — a
firmware revision that ignores the 0x41 repeat mode would otherwise go silent
after 65.5 s with no recovery, and that cannot be verified without the
hardware. Once 0x41 is confirmed on a real wheel, `tick()` can be deleted.

### BUG I — `hat1` was decoded with the wrong semantics

The port stored `hat1 = payload[6] & 0x0F` and documented it as a "4-bit D-pad
code", implying `hat_to_xy()`. But the two hats are encoded differently.
`iforce_report_hats_buttons()` treats hat 0 as an **index** into
`iforce_hat_to_axis[]`, and hat 1 as a **bitmask**: bit3 -> X=-1,
bit1 -> X=+1, bit0 -> Y=-1, bit2 -> Y=+1. Running hat 1 through the direction
table yields garbage (value 1 means Y=-1 as a bitmask but NE as an index).

Added `hat1_to_xy()` implementing the bitmask decode, and documented the
asymmetry at the `DeviceState` declaration. Note the kernel registers
06f8:0004 with `abs_wheel[]`, which has no `ABS_HAT1X/Y`, so upstream never
reports a second hat for this device — but that table entry is flagged `//?`
in iforce-main.c (an admitted guess) and this wheel does physically have two
hats, so the port decodes it regardless.

`hat1` is currently decoded but not consumed by `vigem_gamepad.cpp`, which
only forwards `hat0` to the D-pad.

### Also corrected

- The hat-table comment claimed "0 means neutral / centred". Kernel index 0
  is `{0,-1}` = **North**. The table values were always right (copied
  verbatim); only the comment was wrong. Centred comes from indices 8..15.

### New: `test_protocol.cpp`

First test in the project. Assert-based self-check for the pure decode logic
— no framework, no hardware, no libusb:

```
c++ -std=c++17 test_protocol.cpp iforce_protocol.cpp -o test_protocol && ./test_protocol
```

Covers the hat0 index table (including that index 0 is North, not neutral),
the hat1 bitmask decode (including that it must *not* agree with
`hat_to_xy()`), wheel-packet field offsets, the `len < 7` rejection, and
status-report parsing.

## Open question — shift paddle orientation

`btn_wheel[]` assigns `BTN_GEAR_DOWN` to bit 0 and `BTN_GEAR_UP` to bit 1, and
`vigem_gamepad.cpp` maps bit 0 -> `LEFT_SHOULDER`, bit 1 -> `RIGHT_SHOULDER`.
That matches the usual convention (left paddle downshifts). The kernel only
documents the *semantics* of each bit, not which physical paddle the hardware
wires to bit 0, and the 06f8:0004 table entry is a `//?` guess — so if the
paddles feel reversed in game, swap those two lines.

## Files NOT modified

- `vigem_gamepad.cpp` — left empty as in the original zip. The user
  must drop in their own `vigem_gamepad.h` / `.cpp` (the one that
  was used to build the original release) before re-building.
- `CMakeLists.txt` — unchanged.
- `vcpkg.json` — unchanged.
- `iforce_linux_reference/*` — these are reference-only and not
  part of the build.
- `vigem_gamepad.cpp` — `hat1_to_xy()` is available but not wired up; the
  D-pad still comes from `hat0` only. Wire the second hat here if you want
  it exposed (the Xbox 360 report has no second D-pad, so it would need to
  go to the right thumbstick or spare buttons).

## Build

Same as the original README:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
.\build\Release\iforce_vigem_port.exe
```

## What to look for in the log after this fix

When the device is working correctly, pressing A should now produce
log lines like:

```
debug: test impact pulse triggered
iforce status: effect=0 playing=1 deadman=0 ready=0
iforce status: effect=1 playing=1 deadman=0 ready=16
```

If you instead see:

```
debug A: PERIOD update for large motor failed: libusb_interrupt_transfer (OUT) failed: LIBUSB_ERROR_PIPE
```

then the OUT endpoint has stalled AND the `libusb_clear_halt` retry
also failed — the device itself is in a bad state. Unplug and
re-plug the wheel, then restart the program. If this keeps happening
on every other press, the 20 ms throttle in `on_rumble` may need to
be increased to ~50 ms.
