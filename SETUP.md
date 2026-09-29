# Pigeon Setup (ESP-IDF toolchain, build, flash, provision)

This is the from-scratch path: install the toolchain, build the one firmware
image, flash it to both sticks, give them a shared link key, and designate their
roles. [README.md](README.md) covers what Pigeon is and how to drive it once it
is running.

## Prerequisites (macOS)

- **2 × ESP32-S3 boards** whose USB-C port is wired to the chip's **native USB
  pins (GPIO19/20)**, not a UART bridge chip. A board behind a bridge chip
  cannot be a USB HID device at all. ESP32-S3 SuperNano and Seeed XIAO ESP32-S3
  both work.
- Two data-capable USB cables (charge-only cables are a common time sink).
- Python 3 with `pyserial`: `pip install pyserial`.
- A Mac (the controller) and a target computer.

### Install ESP-IDF

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Every new shell needs the environment sourced before `idf.py` will work:

```bash
. "$HOME/esp/esp-idf/export.sh"
```

## Build and flash

One image runs on both sticks; the role is chosen at runtime, not at build time.
From the project folder:

```bash
. "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py -B build build
idf.py -B build -p /dev/cu.usbmodemXXXX flash
```

Flash both boards with the same image, then repeat for the second one.

Notes:

- List serial ports with `ls /dev/cu.*`.
- `idf.py monitor` attaches the serial console; exit with `Ctrl + ]`.
- **The first flash of a blank board leaves it in download mode. Only a physical
  replug boots it** — esptool cannot reset the board through the firmware's own
  USB CDC. To enter download mode manually, hold BOOT and tap reset.
- **Later reflashes are hands-free.** A board already running Pigeon can reboot
  itself into download mode with `./pigeon-send flashmode` (or `flashmode peer`
  for the paired Pigeon) and boots the new firmware on its own after esptool — no
  buttons, no replug. See [README.md](README.md) "Build and flash".
- Confirm a board is running rather than sitting in download mode:
  `ioreg -c IOHIDInterface -r | grep -c '"Product" = "Pigeon"'` (1 = the Pigeon
  is up). A device named "USB JTAG_serial debug unit" means download mode.

## Provision the link key

The radio is authenticated and encrypted, and **an unkeyed board fails closed —
it pairs with nothing and types nothing.** Both sticks need the *same* key, and
it is only ever carried over USB:

```bash
./pigeon-send gen-key      # once; writes ~/.pigeon_link_key (owner-only)
# plug in the first stick
./pigeon-send set-key
# plug in the second stick
./pigeon-send set-key
```

Check both with `./pigeon-send local-status` — the printed key fingerprints must
match. Do this **before** designating roles: role negotiation travels over the
radio, so an unkeyed board never hears its peer.

## Designate roles

```bash
# with the stick that will live on the Mac plugged in:
./pigeon-send set-nest
```

It reboots as the Nest and announces itself. Plug the other stick into the
**target**; it hears the Nest, makes itself the Pigeon, and reboots once into
keyboard mode. Both remember their roles across power cycles.
`./pigeon-send reset-both` clears the pair so you can swap them.

## Drive it

```bash
./pigeon-send type "Hello, World!"
./pigeon-send key enter
./pigeon-send combo cmd c
./pigeon-send status          # role, link telemetry, key/paired/authfail
```

See [README.md](README.md) for the full command reference.

## Troubleshooting

- **Nothing types, `status` shows `paired=0`.** Check the key first:
  `./pigeon-send local-status` on each stick must print the same fingerprint. A
  missing or mismatched key is the expected cause, because the link fails
  closed by design.
- **`authfail` climbing in `status`.** Packets on the channel are failing
  authentication — usually one stick still holds an old key after a `gen-key
  --force`. Re-run `set-key` on both.
- **Board not found on USB.** `pigeon-send` looks for VID `0x303A`, PID `0x4006`
  (Nest/UNSET) or `0x4005` (Pigeon). A board left in download mode enumerates
  as neither; replug it.
- **Nothing types but the link is paired.** Confirm the Pigeon's USB is in a
  data-capable port on the target and that the target lists a USB keyboard.
- **macOS Keyboard Setup Assistant appears.** Expected the first time macOS sees
  the Pigeon's VID/PID; see README.
- **Flashing fails.** Hold BOOT while starting the flash, release once it begins.
- **`idf.py: command not found`.** Source `export.sh` (above) in this shell.
