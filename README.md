# Pigeon

Pigeon is a **wireless USB-keyboard injector**: it types real USB HID keystrokes
into a target computer, driven from your Mac over a private 2.4 GHz radio link —
no Bluetooth, no helper app on the target, no permissions dance on the Mac.

It is a two-stick set built from two identical ESP32-S3 boards:

```
Mac ──USB(serial)──> NEST  ~~~2.4 GHz (ESP-NOW, ARQ)~~~>  PIGEON ──USB(HID)──> target
```

- **Nest** plugs into the **Mac** and enumerates as a plain USB CDC serial
  device. Any process can open `/dev/cu.usbmodem*` and write to it — there is no
  CoreBluetooth in the path, so no TCC prompt, no entitlement, and no background
  daemon.
- **Pigeon** plugs into the **target** and appears as a normal USB keyboard
  (plus a CDC serial bridge to the target's own serial port).
- The link between them is reliable and ordered (sequence + ACK + retransmit),
  so keystrokes are not dropped even into GUI or password fields.

Typing runs at **~7 ms/char with zero drops**.

> ⚠️ **Authorized use only.** Pigeon injects real keystrokes into a computer over
> the air. Use it only on machines you own or have explicit, documented
> permission to control. You are responsible for complying with all applicable
> laws; the authors provide it for legitimate automation, testing, and research
> and disclaim any liability for misuse.

This README has two halves: **[For humans](#for-humans)** explains how Pigeon
works and how to build, flash, and drive it; **[For AI agents](#for-ai-agents)**
is the short brief for a coding agent that has been handed this repo.

---

# Quickstart

**Have an AI agent set it up for you — no download needed to start.** Paste this
into a coding agent (Claude Code, or any agent with a shell):

```text
I want to set up Pigeon, a wireless USB-keyboard tool. Read this page over the
web and follow it exactly:
https://github.com/cobylai/pigeon-release/blob/main/QUICKSTART.md

Do not clone or download anything to my machine yet. Just read that page (and any
repo pages it points you to) over the web, then do what it says.
```

Use an agent that can run commands on your own machine (the Claude Code desktop
app or CLI, or equivalent) — flashing boards needs shell and USB access, which a
browser-only chat can't do.

The agent reads [QUICKSTART.md](QUICKSTART.md), greets you and offers an overview
or to skip straight to flashing, answers any questions from the repo, and asks
where you want it set up. **Nothing is downloaded until you confirm.** It checks
your dependencies and tests basic communication before relying on the hardware,
then can run the whole build/flash/key/role flow through to a smoke test, or stop
at any checkpoint you name — pausing for you whenever the hardware needs a button
press.

Prefer to do it by hand? Jump to [Build and flash](#build-and-flash) below, or the
step-by-step [SETUP.md](SETUP.md).

---

# For humans

## Hardware

- **2 × ESP32-S3 boards** whose USB-C connector is wired to the chip's **native
  USB pins (GPIO19/20)**, not a UART bridge chip — otherwise the board cannot be
  a USB HID device. ESP32-S3 SuperNano and Seeed XIAO ESP32-S3 both work.
- Two data-capable USB cables.
- A Mac (the controller) and a target computer.

Both sticks are bus-powered; nothing needs a battery.

## Build and flash

Firmware is ESP-IDF, and it's **one image for both boards** — a board decides at
boot whether it's the Nest or the Pigeon (see [Roles](#roles-swappable)):

```bash
. "$HOME/esp/esp-idf/export.sh"
idf.py -B build build                   # build once
idf.py -B build -p <PORT> flash         # same image, flash to each board
```

**First flash** of a blank board needs its buttons: to enter download mode,
**hold BOOT, tap RESET, release BOOT**; after flashing, **physically
unplug/replug** it — an ESP32-S3 stays in download mode until a real power cycle.

**Every flash after that is hands-free.** A board running Pigeon firmware can put
*itself* back into the ROM download bootloader over USB, no buttons and no
replug:

```bash
./pigeon-send flashmode                 # this board (the one on this USB port)
./pigeon-send flashmode peer            # or the paired Pigeon, over the radio
```

The board re-enumerates as VID `0x303A` PID `0x1001`; flash it with esptool and
it boots the new firmware on its own within ~10 s:

```bash
esptool --chip esp32s3 -p <PORT> --before no_reset --after watchdog_reset \
    write_flash @build/flash_args
```

Each board also carries a **unique USB serial number** (derived from its chip
MAC), so when both sticks are plugged into the same Mac you can always tell them
apart by port.

See [SETUP.md](SETUP.md) for one-time ESP-IDF toolchain setup, or
[AI-BUILD.md](AI-BUILD.md) to have an AI agent (Claude Code) do the whole build,
flash, and provisioning for you.

## Link key (do this first)

The radio is an open 2.4 GHz channel. Without a key, anyone in range could both
read every keystroke and inject their own — so the link is authenticated and
encrypted, and **a board with no key fails closed: it pairs with nothing and
types nothing.**

Both sticks must hold the *same* 32-byte key. Provisioning is USB-only and never
crosses the radio:

```bash
./pigeon-send gen-key        # once — mints a key into ~/.pigeon_link_key (0600)
# plug in stick A
./pigeon-send set-key
# plug in stick B
./pigeon-send set-key
```

`./pigeon-send local-status` on each stick prints a short key fingerprint; the
two must match. A mismatched or missing key is the first thing to check when the
link is silent — `./pigeon-send status` reports `key=`, `paired=` and
`authfail=`, and a climbing `authfail` means something on the channel is talking
that cannot authenticate.

Under the hood: HKDF-SHA256 derives a fresh pair of directional session keys for
every pairing, each record is ChaCha20-Poly1305 with the sequence number in the
associated data, and a 64-entry sliding window rejects replays. Pairing HELLOs
carry a key-derived MAC, so a stranger cannot pair with a stick, reset its
sequence space, or flip its role. `./pigeon-send clear-key` forgets a key and
makes a stick re-provisionable.

## Roles (swappable)

The two boards are interchangeable. Which is the Nest and which is the Pigeon is
decided at runtime and remembered in flash (NVS), so you set it up once:

1. A fresh board boots **UNSET** — a plain CDC serial device (no keyboard yet).
2. Plug one into the **Mac** and run `./pigeon-send set-nest`. It becomes the
   Nest and announces itself over the radio.
3. Plug the other into the **target**. It hears the Nest, makes itself the
   **Pigeon**, and reboots once into keyboard mode. Done — both remember their
   roles across every future power-cycle, no re-negotiation.

Confirm the Pigeon is up: `ioreg -c IOHIDInterface -r | grep -c '"Product" = "Pigeon"'`
(1 = running). To reassign, `./pigeon-send reset-both` and designate again.

> Provision both sticks **before** designating roles. Role negotiation travels
> over the radio, so an unkeyed board never hears the Nest and stays UNSET.

## Using it

`pigeon-send` finds the Nest by USB VID/PID and writes framed commands to it —
`pip install pyserial`, then:

```bash
./pigeon-send type "Hello, World!"      # type a string
./pigeon-send type --enter "run this"   # ...and press Enter
./pigeon-send key enter                 # tap a named key
./pigeon-send key up up down enter      # several, in order
./pigeon-send combo cmd c               # Cmd+C
./pigeon-send combo cmd shift 4         # Cmd+Shift+4
./pigeon-send read 3                    # read 3s of the target's serial output
./pigeon-send serial "ls -la"           # send raw bytes to the target's serial port
./pigeon-send profile linux             # set the layout profile
./pigeon-send hidtune 3,3               # tune emitter press,gap in ms
./pigeon-send status                    # role + link telemetry + security state
./pigeon-send local-status              # what the stick on THIS cable holds
./pigeon-send flashmode                 # reboot this board for a hands-free reflash
```

`type` handles all printable ASCII plus space, tab and newline. `key`/`combo`
names: `enter esc backspace tab space caps up down left right home end pageup
pagedown insert delete f1`–`f12` `printscreen scrolllock pause menu`; modifiers
`ctrl shift alt`(`opt`) `cmd`(`gui`/`win`/`super`). Keystrokes land in whatever
window is focused on the target.

### Things you can change

- **Keyboard layout** — `./pigeon-send profile <name>` switches how characters map
  to key codes; the tables live in `main/keymap.c`.
- **Typing speed/reliability** — `./pigeon-send hidtune <press>,<gap>` tunes the
  per-key press and gap in milliseconds without a reflash.
- **Roles** — `./pigeon-send reset-both`, then re-designate; the boards are
  interchangeable.
- **Firmware defaults** — `sdkconfig.defaults` holds the build-time settings (e.g.
  the FreeRTOS tick rate that the typing speed depends on). `sdkconfig` is
  regenerated from it on every build and is not tracked.

### macOS Keyboard Setup Assistant

The first time macOS sees the Pigeon's USB VID/PID it runs its Keyboard Setup
Assistant and withholds input until answered. It asks for the key right of left
shift, then the key left of right shift (ANSI: `z` then `/`). You can answer it
with the board — `./pigeon-send key z`, wait ~4 s, `./pigeon-send key /` — but it
ends on a panel whose **Done** button is a mouse click, which is on you.

## How it works

- `main/` is the firmware, one image for both sticks; `role.c` reads the role
  from NVS at boot and `main.c` picks the USB personality to match. `main.c` is
  transport-agnostic (HID emitter, CDC bridge, deframer, control grammar) and
  talks to a transport through `link.h`; `link_espnow.c` is the radio transport.
  `enow.c` is the ESP-NOW link with auto-pairing and the stop-and-wait ARQ,
  `linkcrypt.c` the key handling and AEAD over it, and `replay.c` the
  anti-replay window (kept dependency-free so `test_replay.c` can exercise it on
  the host).
- Commands travel as framed messages (`[0xAA][type][len][payload][xor]`, see
  `main/frame.h`). `TYPE:` text and `KEY:<mod>,<kc>` taps share one ordered
  emitter queue, so named keys never interleave with typed characters.
- The radio hop carries a sequence number and is ACKed; the receiver only ACKs a
  packet once its buffer accepts it, so a fast sender backpressures instead of
  overrunning the emitter — nothing is silently dropped.
- Every radio packet is authenticated and every payload encrypted under
  per-pairing session keys; an unkeyed board pairs with nothing. See
  **Link key** above.
- The hands-free reflash lives in `main.c` (`flashmode_reboot` and
  `usb_switch_to_cdc_jtag`): the board hands its USB pins from the TinyUSB device
  over to the ROM's CDC+JTAG hardware and forces a bus reset before setting the
  force-download bit, so the host re-enumerates onto the bootloader with no
  physical replug.

## Testing

Two host-side tests run on the Mac without any hardware:

```bash
cc -o /tmp/t test_replay.c main/replay.c -Imain && /tmp/t   # anti-replay window
python3 test_nest_typing.py                                 # end-to-end typing/throughput
```

`test_replay.c` covers the anti-replay logic directly; `test_nest_typing.py` is
the end-to-end typing and throughput harness. `pigeon.py` is the Mac-side framing
+ keymap library that both `pigeon-send` and the harness import.

---

# For AI agents

If you are a coding agent (e.g. Claude Code) that has been handed this repo, this
is everything you need to know to set up the boards and drive the target.

- **A skill is bundled** at `.claude/skills/pigeon/`. Run from the repo root and
  it loads automatically, so you already know the `pigeon-send` commands, the
  link-key model, and the role/flashing flow. Day-to-day "type this on the
  target" requests then just work.

- **To set up boards from scratch, follow [AI-BUILD.md](AI-BUILD.md).** It has a
  paste-ready prompt and the exact order: source ESP-IDF → build once → flash both
  sticks → provision the shared key over USB → designate roles → smoke-test. Do it
  in order and verify each step.

- **The only things you cannot do are physical**, and only on a *blank* board's
  first flash: pressing BOOT/RESET to enter download mode, and the one
  unplug/replug afterward. Stop and ask the human for those, then wait for
  confirmation — never assume a physical action happened.

- **After the first flash, reflashing is hands-free.** Use
  `./pigeon-send flashmode` (the board on this USB port) or
  `./pigeon-send flashmode peer` (the paired Pigeon, over the radio); the board
  re-enumerates as VID `0x303A` PID `0x1001`, then
  `esptool --chip esp32s3 -p <PORT> --before no_reset --after watchdog_reset write_flash @build/flash_args`.
  It boots the new firmware on its own within ~10 s — no replug, so you can flash
  and re-verify without human help.

- **Finding ports.** Normal mode: VID `0x303A`, PID `0x4006` (Nest/UNSET) or
  `0x4005` (Pigeon). Download mode: PID `0x1001`. Each board has a unique USB
  serial, so with both plugged into the same Mac, address one by its port. Two
  boards in the same role share a PID — again, tell them apart by port.

- **The link key is USB-only.** `gen-key`/`set-key` only work over the Nest's USB
  cable and are refused over the radio. Treat `~/.pigeon_link_key` as a secret:
  never print it and never commit it. A board with no key, or a mismatched key,
  fails closed — check `local-status` fingerprints on both sticks first when the
  link is silent.

- **Reflashing does not clear the stored key or role.** A board carrying a stale
  key must be `esptool erase_flash`ed and re-keyed with `set-key`.

- **Verify with the host tests** (no hardware needed) — see
  [Testing](#testing) above.

- **Safe things to change without a reflash:** the layout profile
  (`pigeon-send profile`), the typing timing (`pigeon-send hidtune`), and roles
  (`reset-both`). Layout tables are in `main/keymap.c` and build-time defaults in
  `sdkconfig.defaults` (both need a rebuild + reflash).

- **Authorized use only.** The safety constraint at the top of this file applies
  to you too: only operate targets the human owns or is documented to control.

## License

[MIT](LICENSE) © 2026 Coby Lai.
