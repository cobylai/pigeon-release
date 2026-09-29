// enow: an ESP-NOW link with broadcast auto-pairing and, as of Stage 3, a
// stop-and-wait ARQ that makes the radio hop reliable and ordered. Shared by
// both v2 roles (Nest's CDC relay and Pigeon's transport).
//
// Pairing: on init both sticks broadcast a HELLO beacon on a fixed channel
// until they hear one from a peer, then latch that peer's MAC and go unicast.
//
// ARQ: every enow_send() carries a sequence number and blocks until the peer
// ACKs it (retransmitting on timeout, hard-failing after a bounded number of
// tries). The receiver delivers in order, drops duplicates, and -- crucially --
// only ACKs a DATA packet when the receive callback ACCEPTS it. A callback that
// returns false (its downstream buffer is full) withholds the ACK, so the
// sender pauses and retries: that is the flow-control backpressure that stops a
// fast sender from overrunning a slow consumer (e.g. the HID emitter queue).
//
// Liveness: the controller side sends periodic keepalives so the target side
// can tell an idle link (keepalives still arriving) from a dead one (silence).
//
// Security (Stage 4): every packet is authenticated and every DATA payload is
// encrypted, under session keys derived from a USB-provisioned pre-shared key
// (see linkcrypt.h). HELLOs carry a PSK-keyed MAC, so an unauthenticated
// stranger cannot pair with us, reset our sequence space, or flip our role;
// DATA, ACK and KEEPALIVE are ChaCha20-Poly1305 records with a sliding replay
// window. A board with no PSK fails CLOSED: it pairs with nothing and types
// nothing.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ESP-NOW caps a packet at 250 bytes. enow spends a 5-byte header, a 4-byte
// record counter and a 16-byte Poly1305 tag, so a single send carries at most
// this many payload bytes. Callers fragment above it.
#define ENOW_MAX_PAYLOAD 225

// Delivered-payload callback. Invoked from the ESP-NOW receive context (a
// Wi-Fi task), so keep it short. Return true if the payload was accepted
// (buffered/consumed); return false to withhold the ACK and apply backpressure
// -- the sender will retransmit until it is accepted.
typedef bool (*enow_rx_cb_t)(const uint8_t *data, uint16_t len);

// Peer-role callback: invoked (in the receive context) with a peer's advertised
// role each time a HELLO is parsed. Used to auto-assign an UNSET board.
typedef void (*enow_peer_role_cb_t)(uint8_t peer_role);

// Bring up Wi-Fi (STA, unconnected, power-save off) and ESP-NOW on the shared
// channel, register cb, and start broadcast auto-pairing in the background.
// Non-blocking. Inits NVS and the event loop defensively if the caller has not.
// Also brings up linkcrypt; with no PSK provisioned the radio stays failed
// closed and no pairing will complete.
void enow_init(enow_rx_cb_t cb);

// True once a peer HELLO has been heard, authenticated, and latched -- which
// also means session keys are live. Nothing may be sent or accepted otherwise.
bool enow_paired(void);

// Drop the current pairing and session keys and start hunting for a peer again.
// Used when the link key changes underfoot, since the old session no longer
// derives from the stored PSK.
void enow_repair(void);

// Cumulative count of packets rejected because they failed authentication or
// the replay window -- i.e. traffic that was not from our peer, or was a replay.
// Surfaced by the status command; a climbing number means someone is talking.
uint32_t enow_auth_failures(void);

// Reliably send up to ENOW_MAX_PAYLOAD bytes to the latched peer: blocks until
// the peer ACKs, retransmitting on timeout. Returns false if not paired, the
// length is out of range, or every retry was exhausted (hard delivery failure).
// Does not fragment.
bool enow_send(const uint8_t *data, uint16_t len);

// Start a task that sends a keepalive to the peer every interval_ms while
// paired. Called by the controller side (Nest) so the target (Pigeon) can
// detect a dead link. Idempotent-ish: call once.
void enow_start_keepalive(uint32_t interval_ms);

// Milliseconds since any packet (DATA, ACK, HELLO, or keepalive) was last heard
// from the peer. Used by the target side to lift keys when the link goes quiet.
// Returns a large value before the first packet.
uint32_t enow_ms_since_rx(void);

// Set the role this board advertises in its HELLOs (a byte, matching the app's
// role enum). Default 0 (unset). Safe to call any time.
void enow_set_local_role(uint8_t role);

// Register the peer-role callback (see enow_peer_role_cb_t). Optional.
void enow_set_peer_role_cb(enow_peer_role_cb_t cb);

// Link telemetry for the status command.
const uint8_t *enow_peer_mac(void);   // 6 bytes; valid once paired
int8_t enow_last_rssi(void);          // RSSI (dBm) of the last packet heard
uint32_t enow_retransmits(void);      // cumulative ARQ retransmits
uint32_t enow_rx_drops(void);         // cumulative RX frames dropped on a gap
