// Uplink framing: [0xAA sync][type][len_hi][len_lo][payload][xor cksum]
//
// The uplink carries two kinds of traffic that are otherwise indistinguishable:
// control messages (ASCII KEY:value) and opaque serial bytes forwarded to/from
// the CDC port. A byte of forwarded serial data can look exactly like the start
// of a control message, so every message is wrapped rather than streamed raw.
//
// Length is payload-only, big-endian, and capped at FRAME_MAX_PAYLOAD. The
// checksum is an XOR over everything after the sync byte: type, both length
// bytes, and the payload.
//
// BLE already CRCs and delimits every ATT write, so this layer is redundant on
// the current transport. It is here so the protocol stays transport-agnostic --
// a TCP uplink would need exactly this and gets it for free.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FRAME_SYNC 0xAA
#define FRAME_TYPE_CONTROL 0x01
#define FRAME_TYPE_DATA 0x02

// Bounded by the CDC buffers (512 B) so a full CDC drain fits one frame.
#define FRAME_MAX_PAYLOAD 512
#define FRAME_OVERHEAD 5  // sync + type + 2 len + cksum

typedef enum {
    DF_SYNC = 0,
    DF_TYPE,
    DF_LEN_HI,
    DF_LEN_LO,
    DF_PAYLOAD,
    DF_CKSUM,
} deframer_state_t;

// Called for each frame that passes the sync/type/length/checksum checks.
typedef void (*frame_handler_t)(uint8_t type, const uint8_t *payload, uint16_t len);

typedef struct {
    deframer_state_t state;
    uint8_t type;
    uint16_t len;
    uint16_t got;
    uint8_t cksum;
    frame_handler_t handler;
    uint32_t dropped;  // frames rejected for sync/type/length/checksum
    uint8_t payload[FRAME_MAX_PAYLOAD];
} deframer_t;

void deframer_init(deframer_t *df, frame_handler_t handler);

// Feed one byte. Never blocks and never fails: a malformed frame is abandoned
// and the machine falls back to scanning for the next sync byte, so a corrupt
// stream resynchronizes on its own.
void deframer_feed(deframer_t *df, uint8_t byte);

// Feed a run of bytes.
void deframer_feed_buf(deframer_t *df, const uint8_t *buf, uint16_t len);

// Serialize a frame into out, which must hold len + FRAME_OVERHEAD bytes.
// Returns the total frame length, or 0 if len exceeds FRAME_MAX_PAYLOAD.
uint16_t frame_encode(uint8_t type, const uint8_t *payload, uint16_t len, uint8_t *out);
