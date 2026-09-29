// Transport seam for the Pigeon (target-side) firmware.
//
// The core (main.c) is transport-agnostic: it owns the HID emitter, the CDC
// bridge, the deframer, and the control grammar, and knows nothing about how
// bytes reach the host. A transport implements the half below marked "provided
// by transport"; the core implements the half marked "provided by core" so the
// transport can hand received bytes in and signal that the controller is gone.
//
// Two transports implement this contract:
//   link_ble.c     -- v1's BLE GATT link (legacy/reference).
//   link_espnow.c  -- v2's 2.4 GHz ESP-NOW link (Nest <-> Pigeon).
// Exactly one is compiled per build, chosen by PIGEON_ROLE in CMakeLists.txt.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- provided by the transport ----

// Bring the transport up and start any of its tasks. Called once from app_main.
void link_init(void);

// Largest payload a single uplink frame (board -> host) may carry, after the
// transport's own per-message overhead. The CDC->uplink pump chunks to this.
uint16_t link_max_payload(void);

// Send one already-encoded frame to the host. Returns false if the link is not
// currently able to carry it (no peer, no subscriber, enqueue failed).
bool link_uplink_send(const uint8_t *frame, uint16_t total);

// ---- provided by the core (main.c) ----

// Push received framed bytes into the deframer's input. Returns false if the
// input buffer overflowed, in which case the transport should reset any
// partial-frame tracking it keeps.
bool link_core_deliver_framed(const uint8_t *data, uint16_t len);

// Run a bare legacy keystroke opcode (0x30/0x31/0x32). Only the BLE daemon path
// produces these; the ESP-NOW transport never calls it.
void link_core_legacy_cmd(const uint8_t *data, uint16_t len);

// The controller/peer is gone: drop any queued typing and lift all keys, so a
// lost link never leaves a key held down on the target.
void link_core_controller_lost(void);
