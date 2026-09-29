// Printable-ASCII -> USB HID keycode, US-QWERTY only.
//
// HID transmits scan codes, not characters: the host applies its own keymap on
// receipt. So this table is only correct when the target computer is set to
// US-QWERTY. Under a different layout the letters still land but the symbols
// will not -- which matters, because that is exactly where passwords live.
//
// Making this layout-aware is Step 3 work: `current_profile` is the field it
// will hang off. Deliberately no translation attempted here.
//
// This duplicates the map in pigeon.py. The duplication is intentional: it lets
// a whole string cross the uplink as one TYPE: frame instead of one BLE
// round-trip per character (~185 ms each). If you change one, change both.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KEYMAP_SHIFT 0x80

// Returns (KEYMAP_SHIFT | keycode) for a supported character, or 0 if the
// character has no US-QWERTY keystroke.
uint8_t keymap_lookup(char c);
