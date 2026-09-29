#include "enow.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linkcrypt.h"
#include "nvs_flash.h"

static const char *TAG = "enow";

// Both sticks must agree on this channel; ESP-NOW has no channel negotiation.
#define ENOW_CHANNEL 6

// 5-byte header: magic 'P','G', version, type, seq. The magic keeps stray
// ESP-NOW traffic out; type separates control from data; seq drives the ARQ.
// Version 3 is the Stage 4 authenticated format -- bumped so a board still
// running the plaintext version 2 format is ignored outright rather than
// half-parsed.
#define ENOW_MAGIC0 'P'
#define ENOW_MAGIC1 'G'
#define ENOW_VERSION 3
#define ENOW_HDR_LEN 5
#define ENOW_TYPE_HELLO 0x01
#define ENOW_TYPE_DATA 0x02
#define ENOW_TYPE_ACK 0x03
#define ENOW_TYPE_KEEPALIVE 0x04

// Record layout for DATA/ACK/KEEPALIVE:
//   header(5) | counter(4, big-endian) | ciphertext(n) | tag(16)
// The header and counter are the AEAD's associated data, so the type and the
// ARQ sequence number are authenticated even though they travel in the clear.
#define ENOW_CTR_LEN 4
#define ENOW_REC_OVERHEAD (ENOW_HDR_LEN + ENOW_CTR_LEN + LINKCRYPT_TAG_LEN)

// HELLO layout (never encrypted -- it is what establishes the session key):
//   header(5) | role(1) | our random(16) | echoed random(16) | MAC(8)
// The echoed random is what makes re-pairing safe: a directed reply is only
// accepted if it echoes the random we most recently broadcast, so a stale reply
// can never be paired with a newer random and derive a mismatched key.
#define ENOW_HELLO_BODY (ENOW_HDR_LEN + 1 + LINKCRYPT_RAND_LEN + LINKCRYPT_RAND_LEN)
#define ENOW_HELLO_LEN (ENOW_HELLO_BODY + LINKCRYPT_HELLO_TAG_LEN)

// Stop-and-wait ARQ timing. A present peer ACKs within a round trip (~1-2 ms),
// so 20 ms per attempt is generous headroom. We retransmit against a TIME budget
// rather than a fixed try count: a receiver that is merely backpressured (its
// downstream buffer full) withholds ACKs until it drains, which for one frame's
// worth of characters is at most ~(225 * emitter ms/char) -- comfortably under
// the deadline. Only a genuinely gone peer runs out the clock; that tears the
// link down so a fresh pairing resyncs the sequence spaces (see enow_send).
#define ACK_TIMEOUT_MS 20
#define SEND_DEADLINE_MS 3000

// If this many records in a row fail to authenticate, our session key almost
// certainly disagrees with the peer's (see the s_self_rand race note in
// recv_cb). Tear down rather than stalling until the 3 s send deadline.
#define AUTH_FAIL_LIMIT 8

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static enow_rx_cb_t s_rx_cb;
static volatile bool s_paired;
static uint8_t s_peer_mac[6];
static uint8_t s_self_mac[6];

// Role negotiation: this board advertises s_local_role in every HELLO, and
// reports a peer's advertised role through s_peer_role_cb so the app can
// auto-assign an UNSET board. Both default to "no role / no callback".
static volatile uint8_t s_local_role;
static enow_peer_role_cb_t s_peer_role_cb;

// The random this board is currently offering. Regenerated for every pairing
// attempt so that no two pairings ever derive the same session key -- which is
// what lets each session restart its record counter from zero without ever
// reusing an AEAD nonce.
static uint8_t s_self_rand[LINKCRYPT_RAND_LEN];

// Link telemetry, surfaced by the status command: RSSI of the last packet heard,
// the count of ARQ retransmits, the count of received DATA frames dropped for a
// sequence gap, and the count of packets that failed authentication or replay.
// All best-effort diagnostics.
static volatile int8_t s_last_rssi;
static volatile uint32_t s_retransmits;
static volatile uint32_t s_rx_drops;
static volatile uint32_t s_auth_failures;
static volatile uint32_t s_auth_fail_run;

// ARQ state. tx side is driven from a single caller task (enow_send); rx side
// and ACK signalling run in the ESP-NOW receive context.
static uint8_t s_tx_seq;                    // next DATA seq to send
static uint8_t s_rx_expected;               // next DATA seq expected from peer
static SemaphoreHandle_t s_ack_sem;         // given when the awaited ACK lands
static volatile uint8_t s_awaiting_seq;
static volatile bool s_awaiting_ack;
static SemaphoreHandle_t s_tx_lock;         // serialize enow_send callers

static volatile int64_t s_last_rx_us;

static void fill_header(uint8_t *buf, uint8_t type, uint8_t seq) {
    buf[0] = ENOW_MAGIC0;
    buf[1] = ENOW_MAGIC1;
    buf[2] = ENOW_VERSION;
    buf[3] = type;
    buf[4] = seq;
}

static void put_ctr(uint8_t *buf, uint32_t ctr) {
    buf[0] = (uint8_t)(ctr >> 24);
    buf[1] = (uint8_t)(ctr >> 16);
    buf[2] = (uint8_t)(ctr >> 8);
    buf[3] = (uint8_t)ctr;
}

static uint32_t get_ctr(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8)
           | (uint32_t)buf[3];
}

static bool add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = ENOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    // ESP-NOW's own AES link encryption stays off deliberately: it is capped at
    // a handful of encrypted peers and its key handling is awkward, so we layer
    // ChaCha20-Poly1305 ourselves instead.
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_peer failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Build one sealed record into pkt. Returns the total length, or 0 on failure
// (no session key). Every call draws a FRESH counter -- including retransmits,
// which must never reuse a counter the receiver's replay window may have
// already advanced past.
static uint16_t build_record(uint8_t *pkt, size_t cap, uint8_t type, uint8_t seq,
                             const uint8_t *payload, uint16_t len) {
    if (cap < ENOW_REC_OVERHEAD + len) {
        return 0;
    }
    uint32_t ctr = linkcrypt_next_ctr();
    fill_header(pkt, type, seq);
    put_ctr(pkt + ENOW_HDR_LEN, ctr);

    size_t sealed = 0;
    if (!linkcrypt_seal(ctr, pkt, ENOW_HDR_LEN + ENOW_CTR_LEN, payload, len,
                        pkt + ENOW_HDR_LEN + ENOW_CTR_LEN, cap - ENOW_HDR_LEN - ENOW_CTR_LEN,
                        &sealed)) {
        return 0;
    }
    return (uint16_t)(ENOW_HDR_LEN + ENOW_CTR_LEN + sealed);
}

// Small control packets (ACK, KEEPALIVE) are fire-and-forget: no ARQ. They
// carry no payload but are still sealed, so a stranger cannot forge an ACK and
// make us believe an undelivered frame landed.
static void send_ctrl(const uint8_t *dst, uint8_t type, uint8_t seq) {
    uint8_t pkt[ENOW_REC_OVERHEAD];
    uint16_t total = build_record(pkt, sizeof(pkt), type, seq, NULL, 0);
    if (total > 0) {
        esp_now_send(dst, pkt, total);
    }
}

// HELLO carries this board's role (for role negotiation) and the random half of
// the session-key derivation, authenticated with the PSK-derived HELLO key. The
// source MAC goes into the MAC'd body so a captured HELLO cannot be replayed
// from a different address.
static void send_hello(const uint8_t *dst, const uint8_t *echo_rand) {
    if (!linkcrypt_has_psk()) {
        return;  // failed closed: unprovisioned boards do not advertise
    }
    uint8_t pkt[ENOW_HELLO_LEN];
    fill_header(pkt, ENOW_TYPE_HELLO, 0);
    pkt[ENOW_HDR_LEN] = s_local_role;
    memcpy(pkt + ENOW_HDR_LEN + 1, s_self_rand, LINKCRYPT_RAND_LEN);
    if (echo_rand != NULL) {
        memcpy(pkt + ENOW_HDR_LEN + 1 + LINKCRYPT_RAND_LEN, echo_rand, LINKCRYPT_RAND_LEN);
    } else {
        memset(pkt + ENOW_HDR_LEN + 1 + LINKCRYPT_RAND_LEN, 0, LINKCRYPT_RAND_LEN);
    }

    // MAC covers the body plus our own address.
    uint8_t signed_body[ENOW_HELLO_BODY + 6];
    memcpy(signed_body, pkt, ENOW_HELLO_BODY);
    memcpy(signed_body + ENOW_HELLO_BODY, s_self_mac, 6);
    if (!linkcrypt_hello_tag(signed_body, sizeof(signed_body), pkt + ENOW_HELLO_BODY)) {
        return;
    }
    esp_now_send(dst, pkt, sizeof(pkt));
}

static void reset_arq(void) {
    s_tx_seq = 0;
    s_rx_expected = 0;
    s_awaiting_ack = false;
    s_auth_fail_run = 0;
}

// force=false: latch only if this is a new peer (leave an established peer's
// sequence space alone). force=true: (re)latch and reset the sequence space even
// if it is already our peer -- used when the peer announces a reboot.
static void latch_peer(const uint8_t *mac, bool force) {
    bool same = s_paired && memcmp(mac, s_peer_mac, 6) == 0;
    if (same && !force) {
        return;
    }
    if (!add_peer(mac)) {
        return;
    }
    memcpy(s_peer_mac, mac, 6);
    reset_arq();  // fresh sequence space for a fresh (or rebooted) pairing
    s_paired = true;
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x",
             same ? "peer rebooted; re-synced" : "paired with",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void drop_link(const char *why) {
    ESP_LOGW(TAG, "%s; dropping link", why);
    s_paired = false;
    linkcrypt_session_end();
}

// Verify a HELLO's MAC and hand back its fields. Returns false for anything not
// signed by a holder of our PSK -- which is the whole point: an unauthenticated
// stranger must not be able to pair with us, reset our ARQ, or set our role.
static bool parse_hello(const uint8_t *data, int len, const uint8_t *src, uint8_t *role_out,
                        const uint8_t **rand_out, const uint8_t **echo_out) {
    if (len < ENOW_HELLO_LEN) {
        return false;
    }
    uint8_t signed_body[ENOW_HELLO_BODY + 6];
    memcpy(signed_body, data, ENOW_HELLO_BODY);
    memcpy(signed_body + ENOW_HELLO_BODY, src, 6);
    if (!linkcrypt_hello_verify(signed_body, sizeof(signed_body), data + ENOW_HELLO_BODY)) {
        return false;
    }
    *role_out = data[ENOW_HDR_LEN];
    *rand_out = data + ENOW_HDR_LEN + 1;
    *echo_out = data + ENOW_HDR_LEN + 1 + LINKCRYPT_RAND_LEN;
    return true;
}

static void handle_hello(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const uint8_t *src = info->src_addr;
    uint8_t peer_role = 0;
    const uint8_t *peer_rand = NULL;
    const uint8_t *echo = NULL;
    if (!parse_hello(data, len, src, &peer_role, &peer_rand, &echo)) {
        s_auth_failures++;
        return;
    }
    s_last_rx_us = esp_timer_get_time();

    // A BROADCAST hello means the peer is (re)booting or still searching: it
    // restarted its sequence space at 0, so we must reset ours too, even if it
    // is already our peer -- otherwise a one-sided reboot desyncs the ARQ and
    // the next line is lost until a hard-fail re-pair. We answer with a DIRECTED
    // reply that echoes their random, so they can tell our reply apart from a
    // stale one and both sides derive the same session key.
    //
    // A DIRECTED hello is that reply: it must echo the random we are currently
    // offering, or it belongs to an older attempt and is ignored. We do NOT
    // reply to it, or two peers would ping-pong hellos (and resets) forever.
    bool to_broadcast = (memcmp(info->des_addr, BROADCAST_MAC, 6) == 0);
    if (to_broadcast) {
        // Derive against the random we are currently offering -- deliberately
        // NOT a freshly generated one. When both boards broadcast at the same
        // moment they each answer the other, and if each rolled a new random on
        // receipt they would derive against the other's superseded value and
        // never agree. Keeping ours fixed makes both sides sort the same pair.
        if (!linkcrypt_session_begin(s_self_rand, peer_rand)) {
            return;
        }
        latch_peer(src, true);
        send_hello(src, peer_rand);
    } else {
        if (memcmp(echo, s_self_rand, LINKCRYPT_RAND_LEN) != 0) {
            return;  // reply to a superseded broadcast; a newer one is in flight
        }
        if (!linkcrypt_session_begin(s_self_rand, peer_rand)) {
            return;
        }
        latch_peer(src, true);
    }

    // Report the peer's role so an UNSET board can adopt the complement.
    // Runs on every HELLO, so a missed announcement self-heals on the next.
    if (s_peer_role_cb) {
        s_peer_role_cb(peer_role);
    }
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < ENOW_HDR_LEN || data[0] != ENOW_MAGIC0 || data[1] != ENOW_MAGIC1
        || data[2] != ENOW_VERSION) {
        return;  // not ours
    }
    const uint8_t *src = info->src_addr;
    if (memcmp(src, s_self_mac, 6) == 0) {
        return;  // our own broadcast
    }
    if (info->rx_ctrl != NULL) {
        s_last_rssi = (int8_t)info->rx_ctrl->rssi;
    }

    uint8_t type = data[3];
    uint8_t seq = data[4];

    if (type == ENOW_TYPE_HELLO) {
        handle_hello(info, data, len);
        return;
    }

    if (!s_paired || memcmp(src, s_peer_mac, 6) != 0) {
        return;  // only talk to our latched peer
    }

    // Everything past here is a sealed record. Authenticate before believing a
    // single field of it -- including the type and sequence number, which are in
    // the clear but covered by the AEAD's associated data.
    if (len < (int)ENOW_REC_OVERHEAD) {
        s_auth_failures++;
        return;
    }
    uint32_t ctr = get_ctr(data + ENOW_HDR_LEN);
    const uint8_t *sealed = data + ENOW_HDR_LEN + ENOW_CTR_LEN;
    size_t sealed_len = (size_t)len - ENOW_HDR_LEN - ENOW_CTR_LEN;

    uint8_t plain[ENOW_MAX_PAYLOAD];
    size_t plain_len = 0;
    if (!linkcrypt_open(ctr, data, ENOW_HDR_LEN + ENOW_CTR_LEN, sealed, sealed_len, plain,
                        sizeof(plain), &plain_len)) {
        s_auth_failures++;
        // A run of these means our session key disagrees with the peer's, which
        // only a re-pair can fix. Bail out early instead of stalling on ACKs
        // that can never arrive.
        if (++s_auth_fail_run >= AUTH_FAIL_LIMIT) {
            drop_link("repeated authentication failures");
        }
        return;
    }
    // Replay check only AFTER the tag verifies, so forged counters can never
    // advance our window and lock out the real peer.
    if (!linkcrypt_replay_ok(ctr)) {
        s_auth_failures++;
        return;
    }
    s_auth_fail_run = 0;
    s_last_rx_us = esp_timer_get_time();

    if (type == ENOW_TYPE_KEEPALIVE) {
        return;  // liveness only; timestamp already updated
    }

    if (type == ENOW_TYPE_ACK) {
        if (s_awaiting_ack && seq == s_awaiting_seq) {
            xSemaphoreGive(s_ack_sem);
        }
        return;
    }

    if (type == ENOW_TYPE_DATA) {
        if (seq == s_rx_expected) {
            bool accepted = s_rx_cb && plain_len > 0
                                ? s_rx_cb(plain, (uint16_t)plain_len)
                                : false;
            if (accepted) {
                s_rx_expected = (uint8_t)(seq + 1);
                send_ctrl(src, ENOW_TYPE_ACK, seq);
            }
            // else: withhold the ACK -- backpressure. Sender retransmits.
        } else if (seq == (uint8_t)(s_rx_expected - 1)) {
            // Duplicate: our previous ACK was lost. Re-ACK, do not re-deliver.
            // The counter differs from the original (every record is resealed),
            // so the replay window passes it through and the ARQ sequence number
            // is what identifies it as a duplicate.
            send_ctrl(src, ENOW_TYPE_ACK, seq);
        } else {
            // A gap we cannot fill under stop-and-wait; drop and let the sender
            // retransmit the one we are actually expecting.
            s_rx_drops++;
        }
    }
}

// Persistent: broadcasts HELLO while unpaired, idles while paired. Because it
// never exits, a link torn down by enow_send (peer gone) re-pairs automatically
// when the peer returns -- and re-pairing resets both sequence spaces.
//
// Each broadcast offers a NEWLY generated random, so every pairing attempt
// derives a distinct session key. A directed reply that echoes a superseded
// random is ignored (see handle_hello), which is what keeps that safe.
static void pairing_task(void *arg) {
    (void)arg;
    bool was_paired = false;
    bool warned_unprovisioned = false;
    for (;;) {
        if (!linkcrypt_has_psk()) {
            if (!warned_unprovisioned) {
                ESP_LOGW(TAG, "no link key; radio failed closed until one is provisioned");
                warned_unprovisioned = true;
            }
        } else if (s_paired) {
            was_paired = true;
            warned_unprovisioned = false;
        } else {
            warned_unprovisioned = false;
            if (was_paired) {
                ESP_LOGW(TAG, "link down; re-broadcasting to re-pair");
                was_paired = false;
            }
            linkcrypt_random(s_self_rand, sizeof(s_self_rand));
            send_hello(BROADCAST_MAC, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static uint32_t s_keepalive_ms;
static void keepalive_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_keepalive_ms));
        if (s_paired) {
            send_ctrl(s_peer_mac, ENOW_TYPE_KEEPALIVE, 0);
        }
    }
}

void enow_init(enow_rx_cb_t cb) {
    s_rx_cb = cb;
    s_ack_sem = xSemaphoreCreateBinary();
    s_tx_lock = xSemaphoreCreateMutex();
    s_last_rx_us = 0;

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    // Must come after NVS (the PSK lives there) and before any HELLO goes out.
    linkcrypt_init();

    esp_netif_init();
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // latency over power
    ESP_ERROR_CHECK(esp_wifi_set_channel(ENOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_self_mac));

    // Seed before the receive callback is live: a peer HELLO arriving ahead of
    // pairing_task's first iteration must not derive against an all-zero random.
    linkcrypt_random(s_self_rand, sizeof(s_self_rand));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    if (!add_peer(BROADCAST_MAC)) {
        ESP_LOGE(TAG, "failed to add broadcast peer");
    }

    ESP_LOGI(TAG, "up on channel %d, self %02x:%02x:%02x:%02x:%02x:%02x; pairing",
             ENOW_CHANNEL, s_self_mac[0], s_self_mac[1], s_self_mac[2],
             s_self_mac[3], s_self_mac[4], s_self_mac[5]);
    xTaskCreate(pairing_task, "enow_pair", 3584, NULL, 5, NULL);
}

bool enow_paired(void) {
    return s_paired;
}

void enow_repair(void) {
    drop_link("link key changed");
}

void enow_set_local_role(uint8_t role) {
    s_local_role = role;
}

void enow_set_peer_role_cb(enow_peer_role_cb_t cb) {
    s_peer_role_cb = cb;
}

const uint8_t *enow_peer_mac(void) {
    return s_peer_mac;
}

int8_t enow_last_rssi(void) {
    return s_last_rssi;
}

uint32_t enow_retransmits(void) {
    return s_retransmits;
}

uint32_t enow_rx_drops(void) {
    return s_rx_drops;
}

uint32_t enow_auth_failures(void) {
    return s_auth_failures;
}

bool enow_send(const uint8_t *data, uint16_t len) {
    if (!s_paired || !linkcrypt_session_active() || len == 0 || len > ENOW_MAX_PAYLOAD) {
        return false;
    }

    // One in-flight DATA packet at a time (stop-and-wait), so serialize callers.
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    uint8_t seq = s_tx_seq;
    uint8_t pkt[ENOW_REC_OVERHEAD + ENOW_MAX_PAYLOAD];

    bool ok = false;
    int attempt = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)SEND_DEADLINE_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (attempt++ > 0) {
            s_retransmits++;  // a resend of the same seq
        }
        // Reseal every attempt: the counter must be fresh even for a
        // retransmit, or the peer's replay window -- which a keepalive may have
        // advanced in the meantime -- would reject it and wedge the link.
        uint16_t total = build_record(pkt, sizeof(pkt), ENOW_TYPE_DATA, seq, data, len);
        if (total == 0) {
            break;  // session key vanished underneath us
        }
        xSemaphoreTake(s_ack_sem, 0);  // drain any stale give
        s_awaiting_seq = seq;
        s_awaiting_ack = true;
        esp_now_send(s_peer_mac, pkt, total);
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) == pdTRUE) {
            ok = true;
            break;
        }
    }
    s_awaiting_ack = false;

    if (ok) {
        s_tx_seq = (uint8_t)(seq + 1);
        xSemaphoreGive(s_tx_lock);
        return true;
    }
    // Nothing got through for the whole budget -> the peer is gone, not just
    // backpressured. Tear the link down; the persistent pairing task re-pairs
    // when it returns, and re-pairing resets both sequence spaces so we never
    // wedge on a desynced seq. On the target side, keepalive-liveness has by now
    // lifted any held keys.
    ESP_LOGW(TAG, "seq %u undelivered in %d ms", seq, SEND_DEADLINE_MS);
    drop_link("peer unreachable");
    xSemaphoreGive(s_tx_lock);
    return false;
}

void enow_start_keepalive(uint32_t interval_ms) {
    s_keepalive_ms = interval_ms;
    xTaskCreate(keepalive_task, "enow_keep", 2560, NULL, 4, NULL);
}

uint32_t enow_ms_since_rx(void) {
    int64_t last = s_last_rx_us;
    if (last == 0) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)((esp_timer_get_time() - last) / 1000);
}
