# Building Pigeon with an AI agent

Pigeon is a good fit for an AI coding agent (Claude Code, or any agent with a
shell): the build, flash, key provisioning, and every `pigeon-send` command are
plain terminal commands the agent can run and verify on its own.

The **only** things an agent cannot do are the physical actions — pressing the
board's buttons and replugging USB. This guide makes those hand-offs explicit so
the agent knows exactly when to stop and ask you to touch the hardware.

This repo also bundles a Claude Code skill at `.claude/skills/pigeon/`. When you
run Claude Code from the repo root it loads automatically, so the agent already
knows the `pigeon-send` commands, the link-key model, and the role/flashing flow
before you say a word — the prompt below drives the one-time build; day-to-day
"type this on the target" requests just work.

---

## Paste this prompt into your agent

Copy the block below into Claude Code (run from the repo root, with both sticks
plugged into the Mac over data-capable USB cables):

```text
You are setting up "Pigeon", a two-stick ESP32-S3 wireless USB-keyboard set, on
this Mac. Read README.md and SETUP.md first. Both sticks are plugged in now.

Do this in order, and VERIFY each step before moving on:

1. Toolchain. Ensure ESP-IDF is sourced: `. "$HOME/esp/esp-idf/export.sh"`.
   If idf.py is still missing, follow SETUP.md's "Install ESP-IDF" and stop to
   tell me if anything there needs my input.

2. Build once: `idf.py set-target esp32s3` then `idf.py -B build build`.
   Confirm "Project build complete".

3. Flash both sticks with the SAME image. For each stick:
   - List ports (`ls /dev/cu.usbmodem*`) and identify the target port.
   - Run `idf.py -B build -p <PORT> flash`.
   - IMPORTANT: the FIRST flash of a blank board needs it in download mode, and it
     cannot boot itself afterward. These are PHYSICAL actions only I can do. When
     you need one, STOP and tell me exactly what to press (see "Buttons" in
     AI-BUILD.md), then wait for me to confirm before continuing.
   - After I replug, verify the board booted (not stuck in download mode) with:
     `ioreg -c IOHIDInterface -r | grep -c '"Product" = "Pigeon"'` and
     `ls /dev/cu.usbmodem*`.
   - Once a board runs Pigeon, any LATER reflash is hands-free — see "Reflashing"
     in AI-BUILD.md. You do not need me for those.

4. Provision the shared link key (USB-only; do this BEFORE roles):
   - `./pigeon-send gen-key` once.
   - Plug/confirm one stick, `./pigeon-send set-key`; repeat for the other.
   - Verify both print the SAME fingerprint with `./pigeon-send local-status`.
     (If you can only tell them apart by port, ask me to unplug one at a time.)

5. Designate roles:
   - With the stick that will stay on the Mac connected: `./pigeon-send set-nest`.
   - Tell me to plug the other stick into the TARGET computer. It hears the Nest
     over the radio and reboots into keyboard mode on its own (~5–6 s).
   - Verify: `./pigeon-send status` shows `role=... paired=1` and `authfail=0`.

6. Smoke test: focus a text field on the target, then
   `./pigeon-send type --enter "pigeon online"` and confirm with me that it
   appeared.

Rules:
- NEVER assume a physical action happened. Ask, then wait for my confirmation.
- If a step fails, consult AI-BUILD.md "Common errors" before retrying, and do
  not retry the same failing command more than twice — diagnose instead.
- Treat ~/.pigeon_link_key as a secret: never print it or commit it.
```

---

## Buttons: BOOT and RESET

Every ESP32-S3 board has two buttons, usually labelled **BOOT** (sometimes `IO0`
or `0`) and **RESET** (sometimes `RST`, `EN`, or `R`). The agent cannot press
them; you do. There are only two moves you ever need:

**Enter download (flash) mode — before flashing:**
1. Press and **hold BOOT**.
2. **Tap RESET** once (press and release) while still holding BOOT.
3. **Release BOOT.**

The board is now waiting for a flash. On USB it shows up as **"USB JTAG_serial
debug unit"**, not "Pigeon" — that name is how you and the agent confirm it is in
download mode.

**Boot the new firmware — after flashing:**
- **Physically unplug the USB cable and plug it back in.**

This matters and trips everyone up: after a flash the board **stays in download
mode**. The flasher's "hard reset" over USB does **not** boot it, because the
tool cannot reset the board through the firmware's own USB port. Only a real
power cycle — an actual unplug/replug — starts the new firmware. There is no
software substitute.

**Confirm which mode a board is in (agent can run this):**
```bash
ioreg -c IOHIDInterface -r | grep -c '"Product" = "Pigeon"'   # 1 = running Pigeon firmware
ioreg -p IOUSB -l | grep -c 'JTAG_serial'                     # 1 = stuck in download mode
```

---

## Reflashing (hands-free, after the first flash)

The button dance above is only for a **blank** board's first flash. Once a board
is running Pigeon firmware, it can put *itself* into the ROM download bootloader
over USB — no BOOT/RESET, no replug — so an agent can iterate on firmware without
any help from you.

1. Reboot the board into download mode:
   - `./pigeon-send flashmode` — the board on this USB port (the Nest/UNSET), or
   - `./pigeon-send flashmode peer` — the paired Pigeon, asked over the radio.
2. It re-enumerates as **VID `0x303A` PID `0x1001`**. Find that port the way
   `pigeon-send` finds its normal ports — scan for `vid == 0x303A and pid == 0x1001`.
3. Flash it directly with esptool:
   ```bash
   esptool --chip esp32s3 -p <PORT> --before no_reset --after watchdog_reset \
       write_flash @build/flash_args
   ```
4. The board is back on its normal PID (Nest `0x4006`, Pigeon `0x4005`) within
   about 10 s, no replug. Confirm it's alive with `./pigeon-send status`.

Quirks:
- Flashing does **not** clear the stored key or role. A board carrying a stale
  key must be `esptool erase_flash`ed and re-keyed with `./pigeon-send set-key`.
- Each board has a **unique USB serial**, and two boards in the same role share a
  PID — so when both are attached, address one by its port.

---

## Common errors

| Symptom | Cause | Fix |
|---|---|---|
| `idf.py: command not found` | ESP-IDF not sourced in this shell | `. "$HOME/esp/esp-idf/export.sh"` (needed once per shell) |
| Port shows as `USB JTAG_serial debug unit`, not Pigeon | Board is in download mode after a flash | **Physically unplug/replug** the board |
| `flash` fails to connect / times out | Board not in download mode | Enter download mode (**hold BOOT, tap RESET, release BOOT**), then flash |
| Board not found on USB at all | Charge-only cable, or wrong port | Use a **data** USB cable; `pigeon-send` looks for VID `0x303A`, PID `0x4006` (Nest/UNSET) or `0x4005` (Pigeon) |
| `set-key` succeeds but `local-status` says `no reply` | Board rebooted / port dropped briefly | Wait a few seconds and retry; re-confirm the port with `ls /dev/cu.usbmodem*` |
| `status` shows `paired=0`, nothing types | Missing or mismatched link key (fails closed by design) | `local-status` on each stick — fingerprints **must match**; re-run `gen-key` + `set-key` on both if not |
| `authfail` climbing in `status` | Something on the channel can't authenticate — usually one stick on an old key | Re-run `./pigeon-send set-key` on **both** sticks with the same key |
| `set-nest` done, but the other stick never becomes the Pigeon | Peer wasn't keyed (role negotiation rides the encrypted radio), or is unpowered/out of range | Provision **both** keys **before** roles; confirm the peer is powered and near |
| Both sticks end up `role=unset` after `reset-both` | Expected — `reset-both` clears both roles | Re-designate: `./pigeon-send set-nest`, then plug the other into the target |
| Typing works but a macOS "Keyboard Setup Assistant" popped up | First time macOS sees the Pigeon's USB VID/PID | `./pigeon-send key z`, wait ~4 s, `./pigeon-send key /`; then **you** click **Done** (a mouse click the board can't make) |
| Keystrokes land in the wrong app | Nothing enforces focus on the target | Make sure the intended window is frontmost on the **target** before typing |

For the full command reference and the security model, see
[README.md](README.md); for from-scratch toolchain install, [SETUP.md](SETUP.md).
