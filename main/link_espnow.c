// link_espnow: the Pigeon-side v2 transport. Implements link.h over the enow
// module, so the transport-agnostic core (main.c) drives typing over the
// 2.4 GHz radio instead of BLE.
//
// Received ESP-NOW payloads are framed bytes relayed by Nest -> fed into the
// deframer. Delivery reports acceptance back to enow so the ARQ can apply
// backpressure: if the deframer's input buffer is full, we return false, enow
// withholds the ACK, and Nest pauses. Uplink frames (board -> host) go back
// over the radio.
//
// Liveness (Stage 3): Nest sends keepalives, so silence genuinely means the
// controller is gone (unlike Stage 2's data-silence watchdog, which fired
// mid-type because a long line's own typing produced no radio traffic). Here we
// lift keys only after keepalives AND data both stop for LIVENESS_TIMEOUT_MS.

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "enow.h"
#include "frame.h"
#include "link.h"

static const char *TAG = "link_enow";

// Keepalives arrive every 200 ms while the controller is alive, so ~3.5 missed
// in a row means it is really gone. Long enough to ride out a brief RF hiccup,
// short enough that a real unplug lifts keys quickly.
#define LIVENESS_TIMEOUT_MS 700

// Received radio payload -> deframer. Returns whether the bytes were accepted,
// which enow turns into ARQ backpressure (no ACK when the buffer is full).
static bool on_radio_rx(const uint8_t *data, uint16_t len) {
    return link_core_deliver_framed(data, len);
}

uint16_t link_max_payload(void) {
    if (ENOW_MAX_PAYLOAD <= FRAME_OVERHEAD) {
        return 0;
    }
    uint16_t usable = ENOW_MAX_PAYLOAD - FRAME_OVERHEAD;
    return usable > FRAME_MAX_PAYLOAD ? FRAME_MAX_PAYLOAD : usable;
}

bool link_uplink_send(const uint8_t *frame, uint16_t total) {
    return enow_send(frame, total);
}

// Lifts keys once when the controller goes silent, re-arming when it returns.
// Because keepalives keep enow_ms_since_rx() small whenever the controller is
// alive -- even during a long line, whose DATA packets also count -- this never
// fires mid-type.
static void liveness_task(void *arg) {
    (void)arg;
    bool lifted = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!enow_paired()) {
            continue;
        }
        if (enow_ms_since_rx() > LIVENESS_TIMEOUT_MS) {
            if (!lifted) {
                ESP_LOGW(TAG, "controller silent >%d ms; lifting keys", LIVENESS_TIMEOUT_MS);
                link_core_controller_lost();
                lifted = true;
            }
        } else {
            lifted = false;
        }
    }
}

void link_init(void) {
    enow_init(on_radio_rx);
    xTaskCreate(liveness_task, "enow_live", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW transport up (Stage 3: ARQ + keepalive liveness)");
}
