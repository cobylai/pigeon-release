// linkcrypt: the Stage 4 security layer for the ESP-NOW link.
//
// Threat model. The radio is an open 2.4 GHz channel: anyone in range can
// listen and transmit. Without this layer a stranger could both read every
// keystroke and inject their own -- the single most dangerous property a
// wireless keystroke injector could have. This layer makes the link
// confidential, authenticated, and replay-proof against anyone who does not
// hold the pre-shared key.
//
// Key material.
//   PSK       32 bytes, provisioned over USB (never over the air) and persisted
//             in NVS. Both sticks must hold the SAME PSK. See `pigeon-send
//             gen-key` / `set-key`.
//   session   Derived per pairing via HKDF-SHA256 over the PSK, salted with the
//             two 16-byte randoms the sticks exchange in their HELLOs. Yields
//             64 bytes split into two directional keys, so the two directions
//             never share a keystream and each can count from zero
//             independently. Direction is assigned canonically (the stick whose
//             random sorts lower takes the first key) so both sides agree
//             without needing to know each other's role.
//   hello     A separate HMAC-SHA256 key derived from the PSK with a fixed
//             salt/info, so HELLOs can be authenticated BEFORE a session key
//             exists. This is what stops a stranger from forcing a re-pair or
//             flipping a board's role.
//
// Record protection. ChaCha20-Poly1305 AEAD. The nonce is the 4-byte record
// counter zero-padded to 12; because the session key is fresh for every
// pairing, a counter that only ever increases within a session is enough to
// guarantee the nonce is never reused under a given key -- the property whose
// violation would be catastrophic here.
//
// IMPORTANT: every transmitted record must take a FRESH counter, including ARQ
// retransmits (reseal, do not resend the same bytes). The receiver enforces a
// strictly-advancing counter through a sliding replay window, and a keepalive
// sent between a data record and its retransmit would otherwise push the window
// past the retransmit and wedge the link.
//
// Replay. A 64-entry sliding window over the record counter rejects anything at
// or below the window floor and anything already seen, so a captured record
// cannot be re-injected. The ARQ sequence number rides in the AEAD's associated
// data, so it is authenticated too and cannot be rewritten in flight.
//
// Fail closed. With no PSK provisioned there is no session and every record is
// refused in both directions: an unprovisioned board types nothing. That is the
// intended posture -- a board that cannot authenticate its peer must not be a
// keyboard.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKCRYPT_KEY_LEN 32        // PSK and each derived directional key
#define LINKCRYPT_RAND_LEN 16       // per-boot session random, sent in HELLO
#define LINKCRYPT_TAG_LEN 16        // Poly1305 tag on every record
#define LINKCRYPT_HELLO_TAG_LEN 8   // truncated HMAC on every HELLO
#define LINKCRYPT_FP_CHARS 8        // hex chars in a PSK fingerprint

// Bring up PSA crypto and load the PSK from NVS. Safe to call once at boot,
// before the radio. Returns false only if PSA itself failed to start; a missing
// PSK is not an error here, it just leaves the link failed closed.
bool linkcrypt_init(void);

// True once a PSK is present, i.e. this board is provisioned and may pair.
bool linkcrypt_has_psk(void);

// Persist a new 32-byte PSK and adopt it immediately. Ends any live session,
// since the old session key no longer derives from the stored PSK.
bool linkcrypt_set_psk(const uint8_t *psk);

// Forget the PSK. The board falls back to failed-closed and becomes
// re-provisionable over USB.
bool linkcrypt_clear_psk(void);

// Short public fingerprint of the PSK (first 4 bytes of its SHA-256, hex) so
// two boards can be confirmed to match without ever printing the key itself.
// Writes LINKCRYPT_FP_CHARS chars plus a NUL; yields "none" when unprovisioned.
void linkcrypt_fingerprint(char *out, size_t cap);

// Hardware RNG fill, used for session randoms.
void linkcrypt_random(uint8_t *out, size_t len);

// Derive the directional session keys from the PSK and the two exchanged
// randoms, and arm a fresh replay window. Returns false when unprovisioned.
bool linkcrypt_session_begin(const uint8_t *self_rand, const uint8_t *peer_rand);

// True once a session key is live; false means nothing may be sent or accepted.
bool linkcrypt_session_active(void);

// Drop the session keys (peer gone, PSK changed). Pairing derives new ones.
void linkcrypt_session_end(void);

// Next record counter for an outgoing record. Atomic: the ACK path sends from
// the ESP-NOW receive context while data is sent from a caller task, so the two
// must never draw the same counter.
uint32_t linkcrypt_next_ctr(void);

// Seal pt into out as ciphertext||tag (out needs pt_len + LINKCRYPT_TAG_LEN).
// pt_len may be 0 for an authenticated-but-empty record (ACK, keepalive).
bool linkcrypt_seal(uint32_t ctr, const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t pt_len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

// Verify and decrypt a sealed record. Does NOT check the replay window; call
// linkcrypt_replay_ok separately so a forged counter is rejected cheaply first.
bool linkcrypt_open(uint32_t ctr, const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

// Replay window check for a received counter. Returns false if the counter is
// stale or already seen. Only call it AFTER the tag verifies, so an attacker
// cannot advance our window with a forged counter.
bool linkcrypt_replay_ok(uint32_t ctr);

// Authenticate a HELLO body with the PSK-derived HELLO key. Both take the body
// bytes exactly as they go on the wire (header, role, random, source MAC).
bool linkcrypt_hello_tag(const uint8_t *body, size_t len, uint8_t *tag_out);
bool linkcrypt_hello_verify(const uint8_t *body, size_t len, const uint8_t *tag);
