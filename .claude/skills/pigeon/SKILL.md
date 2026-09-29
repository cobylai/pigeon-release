---
name: pigeon
description: Send real hardware USB-keyboard keystrokes to a target computer via the Pigeon ESP32-S3 two-stick radio set, and read/write the target's USB serial port. Use when the user asks to type on / control / send keys to the "target" machine, the board, the ESP32, or "the Pigeon" — anything that must arrive as real USB keyboard input rather than software-injected text — or to read the target's serial console.
---

# Pigeon — hardware keyboard over a 2.4 GHz radio

Two identical ESP32-S3 sticks. **Nest** plugs into this **Mac** as a plain USB
CDC serial device; **Pigeon** plugs into the **target** as a USB HID keyboard
(plus a CDC serial bridge). They auto-pair over a private 2.4 GHz link with a
reliable ACKed protocol.

```
pigeon-send → USB serial → NEST ~~radio (ARQ)~~ PIGEON ─┬─ USB HID → target's focused window
                                                        └─ USB serial ↔ target's /dev/ttyACM* (or COM*)
```

There is **no Bluetooth and no daemon** — the Nest is just a serial port, so any
process may drive it. HID is **output only**; reading from the target goes over
the serial bridge (bidirectional, focus-independent).

`pigeon-send` lives in the repo and needs `pyserial`. It auto-discovers the Nest
by USB VID/PID (0x303A/0x4006); nothing to configure. Run it from the repo dir
(or via its absolute path).

## Commands

```bash
./pigeon-send type "Hello, World!"    # type a string (deletable; no Enter)
./pigeon-send type --enter "cmd"      # ...then press Enter
./pigeon-send key up down enter       # named keys, in order
./pigeon-send combo cmd shift 4       # modifiers… then one final key
./pigeon-send read 8                  # read the target's serial output for 8 s
./pigeon-send serial "hi"             # write text to the target's serial port
./pigeon-send serialhex 68 69         # ...byte-exact from hex
./pigeon-send profile linux           # layout profile (linux/macos/win)
./pigeon-send hidtune 3,3             # tune emitter press,gap (ms), for experiments

# clipboard bridge -- "paste to pigeon" / "copy from pigeon"
./pigeon-send paste                   # type the Mac clipboard onto the target
./pigeon-send paste --enter           # ...and press Enter after
./pigeon-send copy 5                  # 5s of target serial output -> Mac clipboard
```

- **"paste to pigeon"** = `paste`: types the Mac clipboard into the target's
  focused field (ASCII + space/tab/newline; other chars dropped). Good for small
  blocks; make sure the intended target window is focused first.
- **"copy from pigeon"** = `copy N`: collects the target's serial output for N
  seconds and puts it on the Mac clipboard. The target must emit the text to its
  serial console (a command, or `... > /dev/ttyACM*`).

- **Keystrokes land in whatever window is focused on the target.** Nothing
  enforces focus — make sure the intended window is frontmost before typing, or
  keystrokes go to the wrong place.
- `type` covers printable ASCII plus space/tab/newline; unsupported characters
  (emoji, accents) are skipped with a warning. Use `key`/`combo` for named keys
  and shortcuts.
- Typing is reliable (ACKed radio link, no silent drops) at ~7 ms/char.
- `read` prints `[CTRL] …` for control replies and `[DATA] …` for the target's
  serial bytes. Serial output only reaches the port once a reader asserts DTR.

## Link key — check this first when nothing types

The radio is authenticated and encrypted, and **an unkeyed board fails closed:
it pairs with nothing and types nothing.** Both sticks must hold the same key.

- `./pigeon-send status` reports `key=`, `paired=` and `authfail=`. `paired=0`
  with a `key=none` is the expected symptom of an unprovisioned stick, not a
  radio fault.
- `./pigeon-send local-status` asks the stick on THIS cable about itself; run it
  on both and the fingerprints must match.
- Provisioning is USB-only and never crosses the radio: `./pigeon-send gen-key`
  once, then `./pigeon-send set-key` with each stick plugged in. Do this
  **before** designating roles — role negotiation rides the radio, so an unkeyed
  board never hears its peer.
- A climbing `authfail` means packets on the channel are failing
  authentication, usually one stick left on an older key.

## Roles (swappable) / flashing

One firmware image runs on both boards; each decides Nest vs Pigeon at runtime
and remembers it in NVS. A fresh board boots **UNSET** (CDC only, no keyboard).

Set it up once:
- `./pigeon-send set-nest` on the board plugged into the **Mac** → it becomes the
  Nest and announces over the radio.
- The board on the **target** hears it, becomes the **Pigeon**, and reboots once
  into keyboard mode. Both remember their roles across power-cycles.
- `./pigeon-send reset-both` clears both (for swapping), then designate again.

Flashing: same image to each board (`idf.py -B build -p <PORT> flash`). The
**first** flash of a blank board needs the buttons — enter download mode with
**BOOT held + RESET**, and **physically replug** after flashing (the ESP32-S3
stays in download mode until a real power cycle). These are the only physical
actions; stop and ask the human, then wait for confirmation.

**Reflashing without buttons:** a board already running Pigeon can reboot itself
into the ROM download bootloader — `./pigeon-send flashmode` (this board) or
`./pigeon-send flashmode peer` (the paired Pigeon, over the radio). It
re-enumerates as VID 0x303A PID 0x1001; then
`esptool --chip esp32s3 -p <PORT> --before no_reset --after watchdog_reset write_flash @build/flash_args`
and it boots the new firmware on its own within ~10 s, no replug. Quirks:
flashing does NOT clear the stored key or role (a stale-keyed board needs
`esptool erase_flash` then `set-key`); each board has a unique USB serial and two
boards in the same role share a PID, so address one by port when both are
attached.

Verify the Pigeon booted: `ioreg -c IOHIDInterface -r | grep -c '"Product" = "Pigeon"'`
(1 = up). Nest/UNSET enumerate as PID 0x4006, the Pigeon as 0x4005. See
`README.md` for the full command reference and `AI-BUILD.md` for agent-driven
setup and a common-errors table.
