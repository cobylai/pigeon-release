#include "role.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tusb.h"

static const char *TAG = "role";

// Shares the "cfg" namespace with the keyboard-layout profile; distinct key.
#define NVS_NAMESPACE "cfg"
#define NVS_KEY_ROLE "role"

board_role_t role_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ROLE_UNSET;  // never written
    }
    uint8_t stored = ROLE_UNSET;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_ROLE, &stored);
    nvs_close(h);
    if (err != ESP_OK || stored > ROLE_PIGEON) {
        return ROLE_UNSET;
    }
    return (board_role_t)stored;
}

bool role_store(board_role_t role) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed; role not persisted");
        return false;
    }
    bool ok = (nvs_set_u8(h, NVS_KEY_ROLE, (uint8_t)role) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "role persisted: %s", role_name(role));
    }
    return ok;
}

void role_apply(board_role_t role) {
    role_store(role);
    ESP_LOGW(TAG, "role set to %s; rebooting to apply", role_name(role));
    // Detach USB cleanly before the reboot. The role change flips the USB PID
    // (0x4005 <-> 0x4006), and a bare esp_restart mid-session left the host
    // seeing the old device never disconnect and the new one never enumerate
    // (board "vanished" until a physical replug). tud_disconnect pulls D+ low so
    // the host tears down the old device first; on restart the new PID
    // enumerates fresh.
    if (tud_inited()) {
        tud_disconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(150));  // let the detach + log + any uplink reply land
    esp_restart();
}

const char *role_name(board_role_t role) {
    switch (role) {
        case ROLE_NEST: return "nest";
        case ROLE_PIGEON: return "pigeon";
        default: return "unset";
    }
}
