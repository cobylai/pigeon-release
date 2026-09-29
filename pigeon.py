#!/usr/bin/env python3
"""Pigeon host library: uplink framing, the keymap, and layout tables.

Imported by `pigeon-send` (the CLI you actually drive the sticks with) and by
`test_nest_typing.py`. It talks to nothing itself -- no serial, no radio, no
Bluetooth -- so it stays importable with nothing plugged in and no extra
dependencies.

The framing here mirrors `main/frame.c`; keep the two in step.
"""

import sys

# ---------------------------------------------------------------------------
# Uplink framing -- mirrors main/frame.c. Keep the two in step.
#
#   [0xAA sync][type][len_hi][len_lo][payload][xor cksum]
#
# Length is payload-only, big-endian. The checksum is an XOR over everything
# after the sync byte.
# ---------------------------------------------------------------------------

FRAME_SYNC = 0xAA
FRAME_TYPE_CONTROL = 0x01
FRAME_TYPE_DATA = 0x02
FRAME_MAX_PAYLOAD = 512
FRAME_OVERHEAD = 5

PROFILES = ("linux", "macos", "win")


def frame_encode(ftype, payload):
    """Wrap payload in a frame. `payload` is bytes; returns bytes."""
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds {FRAME_MAX_PAYLOAD}")
    header = bytes([ftype, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
    cksum = 0
    for b in header + payload:
        cksum ^= b
    return bytes([FRAME_SYNC]) + header + payload + bytes([cksum])


def frame_control(text):
    """A 0x01 frame carrying an ASCII KEY:value control message."""
    return frame_encode(FRAME_TYPE_CONTROL, text.encode("utf-8"))


def frame_data(payload):
    """A 0x02 frame carrying opaque bytes for the target's serial port."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return frame_encode(FRAME_TYPE_DATA, payload)


def frame_corrupt_checksum(frame):
    """Flip every bit of a frame's checksum byte. Used to prove the firmware
    drops the frame and resynchronizes rather than locking up."""
    return frame[:-1] + bytes([frame[-1] ^ 0xFF])


class Deframer:
    """Byte-wise deframer, the mirror of the one in main/frame.c. Feed it bytes
    from notifications; it yields (type, payload) for each frame that passes."""

    SYNC, TYPE, LEN_HI, LEN_LO, PAYLOAD, CKSUM = range(6)

    def __init__(self):
        self.state = self.SYNC
        self.dropped = 0
        self._reset_frame()

    def _reset_frame(self):
        self.type = 0
        self.len = 0
        self.cksum = 0
        self.buf = bytearray()

    def feed(self, data):
        out = []
        for b in data:
            frame = self._feed_byte(b)
            if frame is not None:
                out.append(frame)
        return out

    def _feed_byte(self, b):
        if self.state == self.SYNC:
            if b == FRAME_SYNC:
                self.state = self.TYPE
            return None

        if self.state == self.TYPE:
            if b in (FRAME_TYPE_CONTROL, FRAME_TYPE_DATA):
                self.type = b
                self.cksum = b
                self.state = self.LEN_HI
            elif b == FRAME_SYNC:
                pass  # doubled sync; this byte is the real start of frame
            else:
                self.dropped += 1
                self.state = self.SYNC
            return None

        if self.state == self.LEN_HI:
            self.len = b << 8
            self.cksum ^= b
            self.state = self.LEN_LO
            return None

        if self.state == self.LEN_LO:
            self.len |= b
            self.cksum ^= b
            if self.len > FRAME_MAX_PAYLOAD:
                self.dropped += 1
                self.state = self.SYNC
            else:
                self.buf = bytearray()
                self.state = self.CKSUM if self.len == 0 else self.PAYLOAD
            return None

        if self.state == self.PAYLOAD:
            self.buf.append(b)
            self.cksum ^= b
            if len(self.buf) >= self.len:
                self.state = self.CKSUM
            return None

        # CKSUM
        result = None
        if b == self.cksum:
            result = (self.type, bytes(self.buf))
        else:
            self.dropped += 1
        self.state = self.SYNC
        self._reset_frame()
        return result

# USB HID modifier bitmasks (left-side).
MOD = {
    "ctrl": 0x01, "control": 0x01,
    "shift": 0x02,
    "alt": 0x04, "opt": 0x04, "option": 0x04,
    "gui": 0x08, "cmd": 0x08, "command": 0x08, "win": 0x08, "super": 0x08, "meta": 0x08,
}

# Named non-printable / special keys -> HID keycode.
KEYS = {
    "enter": 0x28, "return": 0x28,
    "esc": 0x29, "escape": 0x29,
    "backspace": 0x2A, "bksp": 0x2A, "delete_back": 0x2A,
    "tab": 0x2B,
    "space": 0x2C,
    "caps": 0x39, "capslock": 0x39,
    "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52,
    "home": 0x4A, "end": 0x4D, "pageup": 0x4B, "pagedown": 0x4E,
    "insert": 0x49, "delete": 0x4C, "del": 0x4C, "forward_delete": 0x4C,
    "f1": 0x3A, "f2": 0x3B, "f3": 0x3C, "f4": 0x3D, "f5": 0x3E, "f6": 0x3F,
    "f7": 0x40, "f8": 0x41, "f9": 0x42, "f10": 0x43, "f11": 0x44, "f12": 0x45,
    "printscreen": 0x46, "scrolllock": 0x47, "pause": 0x48, "menu": 0x65,
}

# Printable ASCII -> (needs_shift, HID keycode).
_ASCII = {}


def _register(chars, base, shift=False):
    for i, ch in enumerate(chars):
        _ASCII[ch] = (shift, base + i)


_register("abcdefghijklmnopqrstuvwxyz", 0x04)
_register("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0x04, shift=True)
# Digit row 1-9 then 0.
_register("1234567890", 0x1E)
# Shifted digit row symbols in the same order as 1..9,0.
_register("!@#$%^&*()", 0x1E, shift=True)
_ASCII[" "] = (False, 0x2C)

# Remaining punctuation: char -> (needs_shift, keycode).
_PUNCT = {
    "-": (False, 0x2D), "_": (True, 0x2D),
    "=": (False, 0x2E), "+": (True, 0x2E),
    "[": (False, 0x2F), "{": (True, 0x2F),
    "]": (False, 0x30), "}": (True, 0x30),
    "\\": (False, 0x31), "|": (True, 0x31),
    ";": (False, 0x33), ":": (True, 0x33),
    "'": (False, 0x34), '"': (True, 0x34),
    "`": (False, 0x35), "~": (True, 0x35),
    ",": (False, 0x36), "<": (True, 0x36),
    ".": (False, 0x37), ">": (True, 0x37),
    "/": (False, 0x38), "?": (True, 0x38),
    "\t": (False, 0x2B),
    "\n": (False, 0x28),
}
_ASCII.update(_PUNCT)

MOD_SHIFT = 0x02

# macOS pops its "Keyboard Setup Assistant" the first time it sees a given
# USB VID/PID, and refuses to pass keystrokes through to anything else until
# the wizard is satisfied. It identifies the layout by asking for two keys
# whose physical position differs between layouts:
#   1. the key immediately to the RIGHT of the LEFT shift
#   2. the key immediately to the LEFT of the RIGHT shift
# name -> (first keycode, second keycode)
KBD_LAYOUTS = {
    "ansi": (0x1D, 0x38),  # z          , /             US and most others
    "iso": (0x64, 0x38),   # non-US \|  , /             European
    "jis": (0x1D, 0x87),   # z          , ro (\|)       Japanese
}

# The assistant animates between the two prompts; sending the second key too
# early means it lands on the old panel and is silently dropped. Measured: a
# 2 s gap was still too short, so leave generous headroom -- this runs once per
# new VID/PID, so a slow answer costs nothing.
KBD_SETUP_PAUSE_S = 4.0


def char_to_report(ch):
    """Return (modifier, keycode) for a printable character, or None if unsupported."""
    entry = _ASCII.get(ch)
    if entry is None:
        return None
    needs_shift, keycode = entry
    return (MOD_SHIFT if needs_shift else 0x00, keycode)
