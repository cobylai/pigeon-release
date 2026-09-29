#!/usr/bin/env python3
"""Stage 2 measurement: end-to-end typing over the ESP-NOW radio.

Drives Nest over USB serial (no daemon, no BLE), which relays to Pigeon over the
radio, which types into a focused terminal. Times each line from serial-write to
when it lands in the capture file, and diffs for drops. This is the Stage 2
go/no-go number: ms/char over the radio vs Stage 0's ~8 ms/char direct.

Setup (both boards plugged into the Mac, paired):
    1. In a terminal:  cat > /tmp/pigeon_capture.txt
    2. Leave it focused.
    3. python3 test_nest_typing.py

Paces one line at a time (waits for each to land before sending the next), so
the 512-char emitter queue never overflows -- Stage 2 has no flow control yet.
macOS-only (osascript focus gate).
"""
import os
import re
import string
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

import pigeon as P

NEST_VID, NEST_PID = 0x303A, 0x4006
CAPTURE = "/tmp/pigeon_capture.txt"
TERMS = {"Terminal", "iTerm2", "Ghostty", "Alacritty", "WezTerm", "kitty", "Warp"}


def frontmost():
    return subprocess.run(
        ["osascript", "-e",
         'tell application "System Events" to get name of first application '
         'process whose frontmost is true'],
        capture_output=True, text=True).stdout.strip()


def gate():
    f = frontmost()
    if f not in TERMS:
        sys.exit(f"ABORT: frontmost is {f!r}, not a terminal -- refusing to type.")


def find_port():
    for p in list_ports.comports():
        if p.vid == NEST_VID and p.pid == NEST_PID:
            return p.device
    sys.exit("ABORT: no Nest found (VID 303A/PID 4006).")


def make_lines():
    """12 unique repeat-dense lines (the same-keycode runs that used to drop),
    each under the frame cap, each with a distinct NN# prefix."""
    L = string.ascii_lowercase
    lines = []
    for i in range(12):
        rot = L[(i * 7) % 26:] + L[:(i * 7) % 26]
        body = "".join(c.upper() + c + c + c.upper() for c in rot)  # AaaA... (104)
        body += "".join(str((i + d) % 10) * 3 for d in range(8))    # digit repeats (24)
        lines.append(f"{i:02d}#{body}")
    return lines


def captured_keys():
    # Read robustly: a cat-owned file can carry stray NULs from an earlier
    # truncation, and lines are NN#body. Regex-extract each NN# record so junk
    # bytes never derail parsing. The harness does NOT truncate the file (that
    # fights cat's open handle) -- start each run with a fresh `cat`.
    try:
        data = open(CAPTURE, "rb").read().decode("latin-1")
    except FileNotFoundError:
        return {}
    out = {}
    for m in re.finditer(r"(\d\d)#([^\n\x00]*)", data):
        out[m.group(1)] = m.group(1) + "#" + m.group(2)
    return out


def main():
    lines = make_lines()
    port = find_port()
    total_chars = sum(len(l) for l in lines)
    print(f"typing {len(lines)} lines ({total_chars} chars) over the radio via {port}")

    per_line = []
    print("focus the `cat` terminal now -- typing starts in 5 s...")
    time.sleep(5)
    with serial.Serial(port, 115200, timeout=1.0, dsrdtr=True) as s:
        s.dtr = True
        time.sleep(0.2)
        for i, line in enumerate(lines):
            gate()
            key = f"{i:02d}"
            t0 = time.time()
            s.write(P.frame_control("TYPE:" + line + "\n"))
            # Wait for this line to land (line-buffered cat flushes on the \n).
            deadline = t0 + 8.0
            while time.time() < deadline and key not in captured_keys():
                time.sleep(0.01)
            dt = time.time() - t0
            landed = key in captured_keys()
            per_line.append((key, len(line), dt, landed))
            print(f"  line {key}: {len(line)} chars in {dt*1000:6.0f} ms"
                  f" = {dt/len(line)*1000:4.1f} ms/char" + ("" if landed else "  MISSING"))

    exp = {l[:l.index("#")]: l for l in lines}
    got = captured_keys()
    total = dropped = 0
    for key, e in exp.items():
        total += len(e)
        g = got.get(key)
        if g is None:
            dropped += len(e)
            continue
        if e != g:
            import difflib
            for tag, i1, i2, _, _ in difflib.SequenceMatcher(
                    None, e, g, autojunk=False).get_opcodes():
                if tag == "delete":
                    dropped += i2 - i1

    typed_chars = sum(n for _, n, _, ok in per_line if ok)
    typed_time = sum(dt for _, _, dt, ok in per_line if ok)
    rate = 100 * dropped / total if total else 0
    print(f"\n{dropped} dropped / {total} chars = {rate:.3f}%")
    if typed_chars:
        print(f"end-to-end throughput: {typed_time/typed_chars*1000:.1f} ms/char"
              f" over {typed_chars} landed chars")
    print("PASS" if dropped == 0 else "drops present (see above)")
    return 0 if dropped == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
