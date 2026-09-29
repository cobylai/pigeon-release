#include "frame.h"

#include <string.h>

void deframer_init(deframer_t *df, frame_handler_t handler) {
    memset(df, 0, sizeof(*df));
    df->handler = handler;
    df->state = DF_SYNC;
}

uint16_t frame_encode(uint8_t type, const uint8_t *payload, uint16_t len, uint8_t *out) {
    if (len > FRAME_MAX_PAYLOAD) {
        return 0;
    }

    out[0] = FRAME_SYNC;
    out[1] = type;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len & 0xFF);
    if (len > 0) {
        memcpy(&out[4], payload, len);
    }

    uint8_t cksum = out[1] ^ out[2] ^ out[3];
    for (uint16_t i = 0; i < len; i++) {
        cksum ^= payload[i];
    }
    out[4 + len] = cksum;

    return (uint16_t)(len + FRAME_OVERHEAD);
}

void deframer_feed(deframer_t *df, uint8_t byte) {
    switch (df->state) {
        case DF_SYNC:
            // Resync point. Everything that is not a sync byte is discarded
            // without comment -- that is the normal cost of recovering from a
            // corrupt frame, not an error worth logging per byte.
            if (byte == FRAME_SYNC) {
                df->state = DF_TYPE;
            }
            break;

        case DF_TYPE:
            if (byte == FRAME_TYPE_CONTROL || byte == FRAME_TYPE_DATA) {
                df->type = byte;
                df->cksum = byte;
                df->state = DF_LEN_HI;
            } else if (byte == FRAME_SYNC) {
                // Doubled sync (0xAA 0xAA ...): treat this byte as the real
                // start of frame and keep waiting for a type. Without this a
                // run of sync bytes would swallow the frame that follows it.
                df->state = DF_TYPE;
            } else {
                df->dropped++;
                df->state = DF_SYNC;
            }
            break;

        case DF_LEN_HI:
            df->len = (uint16_t)(byte << 8);
            df->cksum ^= byte;
            df->state = DF_LEN_LO;
            break;

        case DF_LEN_LO:
            df->len |= byte;
            df->cksum ^= byte;
            if (df->len > FRAME_MAX_PAYLOAD) {
                // Corrupt or hostile length. Drop rather than trust it, so a
                // bad high byte can't park the machine for 64 KB of payload.
                df->dropped++;
                df->state = DF_SYNC;
            } else {
                df->got = 0;
                df->state = (df->len == 0) ? DF_CKSUM : DF_PAYLOAD;
            }
            break;

        case DF_PAYLOAD:
            df->payload[df->got++] = byte;
            df->cksum ^= byte;
            if (df->got >= df->len) {
                df->state = DF_CKSUM;
            }
            break;

        case DF_CKSUM:
            if (byte == df->cksum) {
                if (df->handler != NULL) {
                    df->handler(df->type, df->payload, df->len);
                }
            } else {
                df->dropped++;
            }
            df->state = DF_SYNC;
            break;
    }
}

void deframer_feed_buf(deframer_t *df, const uint8_t *buf, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        deframer_feed(df, buf[i]);
    }
}
