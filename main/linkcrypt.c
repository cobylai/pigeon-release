#include "linkcrypt.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "replay.h"
#include "psa/crypto.h"

static const char *TAG = "linkcrypt";

// Shares the "cfg" namespace with the role and keyboard-layout profile.
#define NVS_NAMESPACE "cfg"
#define NVS_KEY_PSK "linkpsk"

// HKDF labels. Changing either of these invalidates every existing pairing,
// which is exactly what should happen if the record format changes.
static const char HELLO_INFO[] = "pigeon hello key v1";
static const char SESSION_INFO[] = "pigeon session keys v1";
// Fixed salt for the HELLO key: it must derive identically on both boards
// before any random has been exchanged, so there is nothing to salt with.
static const char HELLO_SALT[] = "pigeon hello salt v1";

static uint8_t s_psk[LINKCRYPT_KEY_LEN];
static bool s_has_psk;

static psa_key_id_t s_hello_key;    // HMAC-SHA256 over HELLO bodies
static psa_key_id_t s_tx_key;       // ChaCha20-Poly1305, this board -> peer
static psa_key_id_t s_rx_key;       // ChaCha20-Poly1305, peer -> this board
static bool s_session;

static uint32_t s_tx_ctr;

// Guards the session keys against being destroyed while in use. The link is
// torn down from the ESP-NOW receive callback (a Wi-Fi task) while a send may
// be mid-seal on another task, so key lifetime and key use must not overlap.
static SemaphoreHandle_t s_session_lock;

#define SESSION_LOCK() do { if (s_session_lock) xSemaphoreTake(s_session_lock, portMAX_DELAY); } while (0)
#define SESSION_UNLOCK() do { if (s_session_lock) xSemaphoreGive(s_session_lock); } while (0)

// The (self, peer) random pair the live session was derived from. Re-deriving
// the SAME pair would reset the record counter under an unchanged key and reuse
// AEAD nonces -- the one failure this design must never have -- so a repeat is
// recognised and ignored.
static uint8_t s_session_self_rand[LINKCRYPT_RAND_LEN];
static uint8_t s_session_peer_rand[LINKCRYPT_RAND_LEN];

// Sliding replay window; the logic lives in replay.c so it can be host-tested.
static replay_window_t s_replay;

static void drop_key(psa_key_id_t *id) {
    if (*id != PSA_KEY_ID_NULL) {
        psa_destroy_key(*id);
        *id = PSA_KEY_ID_NULL;
    }
}

// Import raw bytes as a PSA key usable for one algorithm.
static bool import_key(const uint8_t *raw, size_t len, psa_key_type_t type,
                       psa_algorithm_t alg, psa_key_usage_t usage, psa_key_id_t *out) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, type);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_usage_flags(&attr, usage);
    psa_status_t st = psa_import_key(&attr, raw, len, out);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %d", (int)st);
        *out = PSA_KEY_ID_NULL;
        return false;
    }
    return true;
}

// HKDF-SHA256(ikm=s_psk, salt, info) -> out. Requires a PSK.
static bool hkdf(const uint8_t *salt, size_t salt_len, const char *info,
                 uint8_t *out, size_t out_len) {
    if (!s_has_psk) {
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    // PSA requires HKDF inputs in this order: salt, then secret, then info.
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, s_psk,
                                            sizeof(s_psk));
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
                                            (const uint8_t *)info, strlen(info));
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_output_bytes(&op, out, out_len);
    }
    psa_key_derivation_abort(&op);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "HKDF failed: %d", (int)st);
        return false;
    }
    return true;
}

// (Re)derive the HELLO authentication key from the current PSK.
static void rebuild_hello_key(void) {
    drop_key(&s_hello_key);
    if (!s_has_psk) {
        return;
    }
    uint8_t k[LINKCRYPT_KEY_LEN];
    if (hkdf((const uint8_t *)HELLO_SALT, sizeof(HELLO_SALT) - 1, HELLO_INFO, k, sizeof(k))) {
        import_key(k, sizeof(k), PSA_KEY_TYPE_HMAC, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                   PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE, &s_hello_key);
    }
    memset(k, 0, sizeof(k));
}

static void load_psk(void) {
    s_has_psk = false;
    memset(s_psk, 0, sizeof(s_psk));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;  // namespace never written
    }
    size_t len = sizeof(s_psk);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PSK, s_psk, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(s_psk)) {
        s_has_psk = true;
    } else {
        memset(s_psk, 0, sizeof(s_psk));
    }
}

bool linkcrypt_init(void) {
    if (s_session_lock == NULL) {
        s_session_lock = xSemaphoreCreateMutex();
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return false;
    }
    load_psk();
    rebuild_hello_key();
    if (s_has_psk) {
        char fp[LINKCRYPT_FP_CHARS + 1];
        linkcrypt_fingerprint(fp, sizeof(fp));
        ESP_LOGI(TAG, "link key present (fingerprint %s)", fp);
    } else {
        ESP_LOGW(TAG, "NO LINK KEY -- radio failed closed; provision with `pigeon-send set-key`");
    }
    return true;
}

bool linkcrypt_has_psk(void) {
    return s_has_psk;
}

bool linkcrypt_set_psk(const uint8_t *psk) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed; link key not persisted");
        return false;
    }
    bool ok = (nvs_set_blob(h, NVS_KEY_PSK, psk, LINKCRYPT_KEY_LEN) == ESP_OK)
              && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (!ok) {
        return false;
    }
    linkcrypt_session_end();  // old session key no longer derives from the PSK
    memcpy(s_psk, psk, sizeof(s_psk));
    s_has_psk = true;
    rebuild_hello_key();
    return true;
}

bool linkcrypt_clear_psk(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(h, NVS_KEY_PSK);
    bool ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (!ok) {
        return false;
    }
    linkcrypt_session_end();
    memset(s_psk, 0, sizeof(s_psk));
    s_has_psk = false;
    rebuild_hello_key();
    return true;
}

void linkcrypt_fingerprint(char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (!s_has_psk) {
        strlcpy(out, "none", cap);
        return;
    }
    uint8_t digest[32];
    size_t dlen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, s_psk, sizeof(s_psk), digest, sizeof(digest), &dlen)
        != PSA_SUCCESS) {
        strlcpy(out, "error", cap);
        return;
    }
    static const char hex[] = "0123456789abcdef";
    char buf[LINKCRYPT_FP_CHARS + 1];
    for (int i = 0; i < LINKCRYPT_FP_CHARS / 2; i++) {
        buf[i * 2] = hex[digest[i] >> 4];
        buf[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    buf[LINKCRYPT_FP_CHARS] = '\0';
    strlcpy(out, buf, cap);
}

void linkcrypt_random(uint8_t *out, size_t len) {
    esp_fill_random(out, len);
}

bool linkcrypt_session_begin(const uint8_t *self_rand, const uint8_t *peer_rand) {
    // Already running on exactly this pair: keep the session (and its counter)
    // rather than deriving the same key again and restarting from zero, which
    // would reuse nonces we have already spent. Happens when a peer's directed
    // reply is lost and it re-broadcasts the random we already answered.
    if (s_session && memcmp(s_session_self_rand, self_rand, LINKCRYPT_RAND_LEN) == 0
        && memcmp(s_session_peer_rand, peer_rand, LINKCRYPT_RAND_LEN) == 0) {
        return true;
    }
    linkcrypt_session_end();
    if (!s_has_psk) {
        return false;
    }

    // Canonical ordering: both boards must build the same salt and must pick
    // OPPOSITE halves of the derived material, without either knowing the
    // other's role. Sorting the two randoms settles both questions at once.
    int cmp = memcmp(self_rand, peer_rand, LINKCRYPT_RAND_LEN);
    if (cmp == 0) {
        ESP_LOGE(TAG, "peer random equals ours; refusing to derive");
        return false;
    }
    bool self_is_low = cmp < 0;

    uint8_t salt[LINKCRYPT_RAND_LEN * 2];
    memcpy(salt, self_is_low ? self_rand : peer_rand, LINKCRYPT_RAND_LEN);
    memcpy(salt + LINKCRYPT_RAND_LEN, self_is_low ? peer_rand : self_rand, LINKCRYPT_RAND_LEN);

    uint8_t okm[LINKCRYPT_KEY_LEN * 2];
    if (!hkdf(salt, sizeof(salt), SESSION_INFO, okm, sizeof(okm))) {
        return false;
    }
    // The low-random board transmits under the first key and receives under the
    // second; the high-random board does the reverse.
    const uint8_t *tx = self_is_low ? okm : okm + LINKCRYPT_KEY_LEN;
    const uint8_t *rx = self_is_low ? okm + LINKCRYPT_KEY_LEN : okm;

    bool ok = import_key(tx, LINKCRYPT_KEY_LEN, PSA_KEY_TYPE_CHACHA20,
                         PSA_ALG_CHACHA20_POLY1305, PSA_KEY_USAGE_ENCRYPT, &s_tx_key)
              && import_key(rx, LINKCRYPT_KEY_LEN, PSA_KEY_TYPE_CHACHA20,
                            PSA_ALG_CHACHA20_POLY1305, PSA_KEY_USAGE_DECRYPT, &s_rx_key);
    memset(okm, 0, sizeof(okm));
    if (!ok) {
        linkcrypt_session_end();
        return false;
    }

    SESSION_LOCK();
    s_tx_ctr = 0;
    replay_reset(&s_replay);
    memcpy(s_session_self_rand, self_rand, LINKCRYPT_RAND_LEN);
    memcpy(s_session_peer_rand, peer_rand, LINKCRYPT_RAND_LEN);
    s_session = true;
    SESSION_UNLOCK();
    ESP_LOGI(TAG, "session keys derived (%s half)", self_is_low ? "low" : "high");
    return true;
}

bool linkcrypt_session_active(void) {
    return s_session;
}

void linkcrypt_session_end(void) {
    SESSION_LOCK();
    s_session = false;
    drop_key(&s_tx_key);
    drop_key(&s_rx_key);
    s_tx_ctr = 0;
    replay_reset(&s_replay);
    memset(s_session_self_rand, 0, sizeof(s_session_self_rand));
    memset(s_session_peer_rand, 0, sizeof(s_session_peer_rand));
    SESSION_UNLOCK();
}

uint32_t linkcrypt_next_ctr(void) {
    return __atomic_add_fetch(&s_tx_ctr, 1, __ATOMIC_SEQ_CST);
}

// Nonce = 8 zero bytes then the counter, big-endian. Unique per record because
// the counter only ever increases within a session and the session key is fresh
// for every pairing.
static void make_nonce(uint32_t ctr, uint8_t nonce[12]) {
    memset(nonce, 0, 8);
    nonce[8] = (uint8_t)(ctr >> 24);
    nonce[9] = (uint8_t)(ctr >> 16);
    nonce[10] = (uint8_t)(ctr >> 8);
    nonce[11] = (uint8_t)ctr;
}

bool linkcrypt_seal(uint32_t ctr, const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t pt_len,
                    uint8_t *out, size_t out_cap, size_t *out_len) {
    uint8_t nonce[12];
    make_nonce(ctr, nonce);
    // ACKs and keepalives are authenticated but carry nothing. Hand PSA a valid
    // pointer to zero bytes rather than NULL, which it is not obliged to accept.
    static const uint8_t empty;
    if (pt == NULL) {
        pt = &empty;
    }
    SESSION_LOCK();
    psa_status_t st = s_session ? psa_aead_encrypt(s_tx_key, PSA_ALG_CHACHA20_POLY1305, nonce,
                                                   sizeof(nonce), aad, aad_len, pt, pt_len, out,
                                                   out_cap, out_len)
                                : PSA_ERROR_BAD_STATE;
    SESSION_UNLOCK();
    if (st != PSA_SUCCESS) {
        if (st != PSA_ERROR_BAD_STATE) {
            ESP_LOGE(TAG, "seal failed: %d", (int)st);
        }
        return false;
    }
    return true;
}

bool linkcrypt_open(uint32_t ctr, const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len,
                    uint8_t *out, size_t out_cap, size_t *out_len) {
    uint8_t nonce[12];
    make_nonce(ctr, nonce);
    SESSION_LOCK();
    psa_status_t st = s_session ? psa_aead_decrypt(s_rx_key, PSA_ALG_CHACHA20_POLY1305, nonce,
                                                   sizeof(nonce), aad, aad_len, ct, ct_len, out,
                                                   out_cap, out_len)
                                : PSA_ERROR_BAD_STATE;
    SESSION_UNLOCK();
    return st == PSA_SUCCESS;  // a bad tag is expected traffic, not an error to log
}

bool linkcrypt_replay_ok(uint32_t ctr) {
    if (!s_session) {
        return false;
    }
    return replay_accept(&s_replay, ctr);
}

bool linkcrypt_hello_tag(const uint8_t *body, size_t len, uint8_t *tag_out) {
    if (s_hello_key == PSA_KEY_ID_NULL) {
        return false;
    }
    uint8_t full[32];
    size_t full_len = 0;
    if (psa_mac_compute(s_hello_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), body, len, full, sizeof(full),
                        &full_len) != PSA_SUCCESS
        || full_len < LINKCRYPT_HELLO_TAG_LEN) {
        return false;
    }
    memcpy(tag_out, full, LINKCRYPT_HELLO_TAG_LEN);
    return true;
}

bool linkcrypt_hello_verify(const uint8_t *body, size_t len, const uint8_t *tag) {
    uint8_t expect[LINKCRYPT_HELLO_TAG_LEN];
    if (!linkcrypt_hello_tag(body, len, expect)) {
        return false;
    }
    // Constant-time compare: a timing oracle on a MAC check is a real forgery aid.
    uint8_t diff = 0;
    for (size_t i = 0; i < LINKCRYPT_HELLO_TAG_LEN; i++) {
        diff |= (uint8_t)(expect[i] ^ tag[i]);
    }
    return diff == 0;
}
