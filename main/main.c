#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_gpio.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "class/hid/hid_device.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "driver/gpio.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/periph_defs.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_pins.h"
#include "soc/usb_serial_jtag_reg.h"
#include "hal/clk_gate_ll.h"
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"

#include "enow.h"
#include "frame.h"
#include "keymap.h"
#include "link.h"
#include "linkcrypt.h"
#include "role.h"

static const char *TAG = "pigeon";

// This board's role for the session, read once at boot from NVS (see app_main).
static board_role_t s_role;

#define REPORT_ID_KEYBOARD 1
#define MAX_KEYCODES 6
#define KEY_TAP_DELAY_MS 12
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

// Composite layout: CDC (two interfaces, tied together by an IAD) then HID.
// CDC goes first because the IAD must precede the interfaces it associates.
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL,
};

// String descriptor indices, matching usb_string_descriptor below.
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_HID_INTERFACE,
    STRID_CDC_INTERFACE,
};

// The ESP32-S3 USB peripheral has a limited endpoint pool, so these are
// assigned by hand rather than left to chance. HID moved off 0x81 (where it
// sat in the keyboard-only build) to make room for the CDC notification EP.
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_HID_IN 0x83

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

// Filled at boot by build_usb_serial() from the chip's efuse MAC, so it is
// identical across reboots/reflashes (the MAC is fixed in efuse) and differs
// between the two boards. A static array's address is a valid initializer for
// the pointer below, so the descriptor table can reference it before it is
// filled in.
static char s_usb_serial[13] = "000000000000";

static void build_usb_serial(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_usb_serial, sizeof(s_usb_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *usb_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "Coby Lai",
    "Pigeon",
    s_usb_serial,
    "Pigeon Keyboard",
    "Pigeon Serial",
};

// bDeviceClass/SubClass/Protocol must be the Misc/Common/IAD triple
// (0xEF/0x02/0x01). Without it a host binds the whole device to a single class
// driver instead of honouring the IAD, and the composite falls apart.
static const tusb_desc_device_t usb_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    // Interface set changed, so the PID changes with it — hosts cache a driver
    // binding per VID/PID and would otherwise reuse the HID-only one.
    .idProduct = 0x4005,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static const uint8_t usb_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    // TUD_CDC_DESCRIPTOR emits the IAD ahead of the two CDC interfaces itself.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    // Final arg is bInterval, the HID poll interval in 1 ms frames. 1 is the
    // floor for full-speed USB (which is what the ESP32-S3 is). The TYPE:
    // emitter derives its press/gap timing from this (HID_PRESS_MS = poll + 4,
    // HID_GAP_MS = poll * 2), so dropping it from 10 to 1 both speeds typing
    // ~4-5x and gives the fragile all-keys-up report 10x more poll windows to
    // be sampled in. bInterval lives in the endpoint descriptor, not the device
    // descriptor, so VID/PID are unchanged and the Keyboard Setup Assistant is
    // not re-triggered.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, false, sizeof(hid_report_descriptor), EPNUM_HID_IN, 16, 1),
};

// CDC-only personality for the Nest/UNSET role: no HID interface, distinct PID
// so the host never confuses it with the Pigeon keyboard. Selected at boot from
// the NVS role -- USB commits "keyboard or not" at enumeration, so a board picks
// one of these two descriptor sets and keeps it for the session.
#define TUSB_DESC_TOTAL_LEN_CDC (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t usb_device_descriptor_cdc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4006,  // Nest PID (Pigeon is 0x4005)
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static const uint8_t usb_configuration_descriptor_cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_CDC_DATA + 1, 0, TUSB_DESC_TOTAL_LEN_CDC, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

enum {
    // 0x30 <modifier> <keycode...>  press up to 6 keys with a modifier byte, then release.
    CMD_KEY_TAP = 0x30,
    // 0x31 <modifier> <keycode...>  hold keys down (no auto-release).
    CMD_KEY_HOLD = 0x31,
    // 0x32                          release all keys.
    CMD_KEY_RELEASE = 0x32,
};

// ---------------------------------------------------------------------------
// Profile state
// ---------------------------------------------------------------------------

typedef enum {
    PROFILE_LINUX = 0,
    PROFILE_MACOS,
    PROFILE_WIN,
    PROFILE_COUNT,
} profile_t;

static const char *profile_names[PROFILE_COUNT] = {"linux", "macos", "win"};

static profile_t current_profile = PROFILE_LINUX;

#define NVS_NAMESPACE "cfg"
#define NVS_KEY_PROFILE "profile"

static void profile_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;  // never written yet; keep the compiled-in default
    }
    uint8_t stored = 0;
    if (nvs_get_u8(h, NVS_KEY_PROFILE, &stored) == ESP_OK && stored < PROFILE_COUNT) {
        current_profile = (profile_t)stored;
    }
    nvs_close(h);
}

// Mirrored to NVS so the profile survives a replug. Only the profile is
// persisted — TYPE: payloads pass through the queue and are dropped after
// emission. Nothing about them touches flash.
static void profile_store(profile_t p) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "profile: NVS open failed; RAM-only for this boot");
        return;
    }
    if (nvs_set_u8(h, NVS_KEY_PROFILE, (uint8_t)p) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Buffers shared between the pumps
// ---------------------------------------------------------------------------

#define UPLINK_RX_BUF_SZ 1024
#define CDC_TX_RING_SZ 2048
#define HID_QUEUE_LEN 512

// Bounded work per bridge_task iteration, so neither pump can starve the other.
#define UPLINK_RX_CHUNK 256
#define CDC_RX_CHUNK 256
#define CDC_TX_CHUNK 64

#define BRIDGE_TICK_MS 2

// Raw uplink bytes, transport RX -> bridge task.
static StreamBufferHandle_t s_uplink_rx;
// Deframed 0x02 payloads waiting for room in the CDC TX FIFO.
static StreamBufferHandle_t s_cdc_tx;
// HID events (a character to type, or a named-key tap) from the control parser
// to the single emitter task. Routing named keys through the SAME queue as
// typed characters is what keeps them ordered: a tap can never interleave
// between a character's press and release -- which, because HID reports carry
// the full key state, would otherwise disrupt the held key and scramble output.
typedef enum {
    HID_EVT_CHAR = 0,   // type an ASCII byte (press+release)
    HID_EVT_KEYTAP,     // tap a keycode with a modifier (press+release)
    HID_EVT_HOLD,       // press a key and LEAVE it down (single held key)
    HID_EVT_RELEASE,    // release all keys
} hid_evt_kind_t;
typedef struct {
    uint8_t kind;  // hid_evt_kind_t
    uint8_t mod;   // KEYTAP/HOLD: modifier bitmask (unused for CHAR)
    uint8_t code;  // CHAR: the ASCII byte; KEYTAP/HOLD: the HID keycode
} hid_event_t;
static QueueHandle_t s_hid_queue;

static deframer_t s_deframer;

// ---------------------------------------------------------------------------
// HID output
// ---------------------------------------------------------------------------

// Emitter pacing. The ordering guarantee no longer comes from timed delays --
// it comes from tud_hid_report_complete_cb, which fires once the host has
// actually collected a report off the interrupt endpoint. emit_report() sends
// a report and then waits for that completion, so a press is provably on the
// host before its release is sent, and the all-keys-up report between two
// presses of the same keycode is provably delivered before the next press.
// That ordering is what previously had to be approximated with a gap wide
// enough to straddle two poll windows.
//
// The old approach paced with vTaskDelay at a 100 Hz tick, where pdMS_TO_TICKS
// floored every sub-10 ms delay to 0 and snapped the rest to 10 ms -- so the
// "6,8 clean vs 8,10 lossy" cliff was a tick-quantization artifact, not a
// host-sampling floor (press was 0 ms on BOTH sides of it; only the gap crossed
// the 10 ms->0 tick boundary). With completion gating plus a 1 kHz tick,
// press/gap become honest millisecond dwell times layered ON TOP of the
// ordering guarantee rather than being the guarantee. They default
// conservatively and can be swept toward 0 on real hardware via HIDTUNE.
#define HID_PRESS_MS_DEFAULT 3  // extra hold after the host has taken the press
#define HID_GAP_MS_DEFAULT 3    // idle after the host has taken the release
// Volatile: written from the control-frame path, read by the emitter task.
static volatile uint32_t s_hid_press_ms = HID_PRESS_MS_DEFAULT;
static volatile uint32_t s_hid_gap_ms = HID_GAP_MS_DEFAULT;
// Bounds two waits: for the endpoint to free, and for a sent report to be
// collected by the host. A missed completion (host vanished mid-report) must
// not wedge the emitter, so both are bounded and reported, never infinite.
#define HID_READY_TIMEOUT_MS 50
#define HID_COMPLETE_TIMEOUT_MS 50

// Given by tud_hid_report_complete_cb (TinyUSB task context) when a report has
// been collected by the host; waited on by emit_report on the emitter task.
static SemaphoreHandle_t s_hid_report_done;

static bool hid_wait_ready(void) {
    for (int i = 0; i < HID_READY_TIMEOUT_MS; i++) {
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static void send_keyboard_report(uint8_t modifier, const uint8_t *keycodes, uint8_t count) {
    if (!tud_mounted()) {
        return;
    }

    uint8_t report[MAX_KEYCODES] = {0};
    if (count > MAX_KEYCODES) {
        count = MAX_KEYCODES;
    }
    for (uint8_t i = 0; i < count; i++) {
        report[i] = keycodes[i];
    }
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, count > 0 ? report : NULL);
}

// The emitter's report primitive: send one report, then block until the host
// has collected it (or the bounded timeout elapses). Returns true only when the
// report was accepted by the stack AND completed to the host -- so a caller
// that sees true knows the report is delivered, not merely queued. This is the
// ordering guarantee the emitter is built on.
static bool emit_report(uint8_t modifier, const uint8_t *keycodes, uint8_t count) {
    if (!tud_mounted() || !hid_wait_ready()) {
        return false;
    }

    uint8_t report[MAX_KEYCODES] = {0};
    if (count > MAX_KEYCODES) {
        count = MAX_KEYCODES;
    }
    for (uint8_t i = 0; i < count; i++) {
        report[i] = keycodes[i];
    }

    // Drop any stale completion so this wait can only be satisfied by THIS
    // report's callback. Only the emitter task calls emit_report, so no lock is
    // needed -- named keys reach the emitter as queued events, never as a direct
    // call from another task.
    xSemaphoreTake(s_hid_report_done, 0);

    if (!tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, count > 0 ? report : NULL)) {
        return false;
    }
    return xSemaphoreTake(s_hid_report_done, pdMS_TO_TICKS(HID_COMPLETE_TIMEOUT_MS)) == pdTRUE;
}

static void tap_keys(uint8_t modifier, const uint8_t *keycodes, uint8_t count) {
    send_keyboard_report(modifier, keycodes, count);
    vTaskDelay(pdMS_TO_TICKS(KEY_TAP_DELAY_MS));
    send_keyboard_report(0, NULL, 0);
}

// Set when the controller goes away. A queued TYPE: string must not keep
// typing into a machine nobody is driving any more, and any key still held
// must come back up on its own rather than waiting for a human to unplug the
// board. Checked by the emitter, set via link_core_controller_lost() from the
// transport (BLE disconnect, or an ESP-NOW liveness timeout).
static volatile bool s_hid_abort;

// A dropped release is worse than a dropped press: the host keeps the key down
// and turns it into auto-repeat, so one lost report becomes a screenful of
// garbage. Worth retrying, unlike a press.
static void hid_release_all(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (emit_report(0, NULL, 0)) {
            return;
        }
    }
    ESP_LOGW(TAG, "TYPE: release report failed 3x; key may be stuck");
}

// Drains TYPE: characters. Lives in its own task so that a long string paces
// itself at HID speed without stalling either serial pump.
//
// Nothing typed here is echoed back over the uplink. Anything the target
// produces in response comes back the ordinary way, via the CDC->uplink pump.
static void hid_emitter_task(void *arg) {
    (void)arg;
    hid_event_t ev;
    for (;;) {
        // Bounded wait rather than portMAX_DELAY so an abort is still noticed
        // when the queue is empty -- a key can be stuck with nothing queued.
        if (xQueueReceive(s_hid_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_hid_abort) {
                s_hid_abort = false;
                hid_release_all();
            }
            continue;
        }

        if (s_hid_abort) {
            xQueueReset(s_hid_queue);
            s_hid_abort = false;
            hid_release_all();
            ESP_LOGI(TAG, "emit aborted, queue discarded (controller gone)");
            continue;
        }

        // Release-all is a whole event on its own.
        if (ev.kind == HID_EVT_RELEASE) {
            hid_release_all();
            continue;
        }

        uint8_t modifier, keycode;
        if (ev.kind == HID_EVT_KEYTAP || ev.kind == HID_EVT_HOLD) {
            modifier = ev.mod;
            keycode = ev.code;
        } else {
            uint8_t entry = keymap_lookup((char)ev.code);
            if (entry == 0) {
                ESP_LOGW(TAG, "emit: skipping unmappable byte 0x%02X", ev.code);
                continue;
            }
            modifier = (entry & KEYMAP_SHIFT) ? 0x02 : 0x00;
            keycode = entry & 0x7F;
        }

        // Press, provably delivered to the host by emit_report. A press that
        // does not complete is skipped rather than followed by a release, which
        // on its own would read as a phantom keystroke.
        if (!emit_report(modifier, &keycode, 1)) {
            ESP_LOGW(TAG, "emit: press not delivered; skipped");
            continue;
        }
        // HOLD leaves the key down (single held key -- a later event replaces
        // it); release happens on an explicit KEYUP or an abort/mount.
        if (ev.kind == HID_EVT_HOLD) {
            continue;
        }
        // Honest millisecond dwell at the 1 kHz tick; 0 means "rely purely on
        // completion gating". The release is retried because a lost all-keys-up
        // report is what sticks a key down and auto-repeats.
        if (s_hid_press_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_hid_press_ms));
        }
        hid_release_all();
        if (s_hid_gap_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_hid_gap_ms));
        }
    }
}

// ---------------------------------------------------------------------------
// Uplink TX (board -> host)
// ---------------------------------------------------------------------------

// Single-threaded by construction: every caller runs on the bridge task.
static uint8_t s_tx_frame[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];

// Encode a frame and hand it to the transport. Readiness (peer/subscriber
// present) is the transport's concern; link_uplink_send returns false when it
// cannot carry the frame.
static bool uplink_send(uint8_t type, const uint8_t *payload, uint16_t len) {
    uint16_t total = frame_encode(type, payload, len, s_tx_frame);
    if (total == 0) {
        return false;
    }
    return link_uplink_send(s_tx_frame, total);
}

static void uplink_send_control(const char *msg) {
    uplink_send(FRAME_TYPE_CONTROL, (const uint8_t *)msg, (uint16_t)strlen(msg));
}

// ---------------------------------------------------------------------------
// Flash-mode reboot: forced ROM download boot over USB.
//
// This is what makes reflashing hands-free: after the FIRST flash (which still
// needs the buttons), the board can put ITSELF back into the ROM download
// bootloader over USB, so every later flash is button-free and needs no replug.
//
//   pigeon-send flashmode              (this board)  |  flashmode peer (the Pigeon)
//   esptool --chip esp32s3 -p <port> --before no_reset --after watchdog_reset
//       write_flash @flash_args
//
// The hard part is the USB handoff. The ESP32-S3 has two USB paths: the OTG PHY
// that TinyUSB drives for our HID/CDC device, and the ROM's own CDC+JTAG unit
// that the download bootloader speaks. Simply setting RTC_CNTL_FORCE_DOWNLOAD_BOOT
// while TinyUSB still owns the OTG PHY leaves the port dark until a physical
// replug. The fix, in order: tear the OTG PHY down, hand the D+/D- pins to the
// ROM's CDC+JTAG hardware, force a bus reset so the host re-enumerates onto it,
// and only THEN set the force-download bit, right before the reset. (The register
// sequence follows arduino-esp32's usb_persist_restart(RESTART_BOOTLOADER) path.)
// ---------------------------------------------------------------------------

static void IRAM_ATTR flashmode_bus_reset_isr(void *arg) {
    portBASE_TYPE woken = pdFALSE;
    uint32_t status = usb_serial_jtag_ll_get_intsts_mask();
    usb_serial_jtag_ll_clr_intsts_mask(status);
    if (status & USB_SERIAL_JTAG_INTR_BUS_RESET) {
        xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Release the OTG PHY, hand the USB pins to the ROM's CDC+JTAG hardware and
// wait for the host to actually see a bus reset before returning. The wait is
// load-bearing: without it, esp_restart() can write the force-download-boot
// register before the host has re-enumerated onto the CDC+JTAG device, and the
// port goes dark until a physical replug -- exactly what this feature exists to
// avoid.
static void usb_switch_to_cdc_jtag(void) {
    tinyusb_driver_uninstall();  // stops the tinyusb task, usb_del_phy()s the OTG PHY

    periph_ll_reset(PERIPH_MODULE_MAX);
    periph_ll_disable_clk_set_rst(PERIPH_MODULE_MAX);

    // Switch RTC_CNTL's phy select away from the OTG PHY.
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                         (RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE));
    // Do not use an external PHY.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    // Release the D+/D- pins so they can be driven low by hand below.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    // Force the host to see a bus reset (D+/D- both low).
    esp_rom_gpio_pad_select_gpio(USBPHY_DM_NUM);
    esp_rom_gpio_pad_select_gpio(USBPHY_DP_NUM);
    gpio_set_direction(USBPHY_DM_NUM, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(USBPHY_DP_NUM, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(USBPHY_DM_NUM, 0);
    gpio_set_level(USBPHY_DP_NUM, 0);

    const usb_serial_jtag_pull_override_vals_t pull_conf = {.dp_pu = 1, .dm_pu = 0, .dp_pd = 0, .dm_pd = 0};
    usb_serial_jtag_ll_phy_enable_pull_override(&pull_conf);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_BUS_RESET);

    SemaphoreHandle_t reset_sem = xSemaphoreCreateBinary();
    intr_handle_t intr_handle = NULL;
    if (reset_sem != NULL) {
        if (esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, flashmode_bus_reset_isr, reset_sem, &intr_handle) != ESP_OK) {
            vSemaphoreDelete(reset_sem);
            reset_sem = NULL;
            ESP_LOGE(TAG, "FLASHMODE: bus-reset interrupt alloc failed");
        }
    }

    // Reconnect the pins to the hardware CDC+JTAG device now that the reset
    // interrupt is armed.
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    if (reset_sem != NULL) {
        if (xSemaphoreTake(reset_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "FLASHMODE: bus-reset wait timed out");
        }
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
        esp_intr_free(intr_handle);
        vSemaphoreDelete(reset_sem);
    }
}

// Fires late in esp_restart()'s shutdown sequence, right before the actual
// reset -- so the force-download bit is the last thing written.
static void IRAM_ATTR flashmode_shutdown_handler(void) {
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}

// Reboot THIS board into the ESP32-S3's forced ROM download bootloader, so it
// re-enumerates as VID 0x303A PID 0x1001 for esptool. Does not return (on
// success); logs and returns only if the reboot could not be set up.
static void flashmode_reboot(void) {
    ESP_LOGW(TAG, "FLASHMODE: rebooting into ROM download bootloader");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));

    usb_switch_to_cdc_jtag();

    if (esp_register_shutdown_handler(flashmode_shutdown_handler) != ESP_OK) {
        ESP_LOGE(TAG, "FLASHMODE: could not register shutdown handler; aborting reboot");
        return;
    }
    esp_restart();
}

// ---------------------------------------------------------------------------
// Control message grammar: ASCII KEY:value
// ---------------------------------------------------------------------------

static void handle_profile(const char *value) {
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (strcmp(value, profile_names[i]) == 0) {
            current_profile = (profile_t)i;
            profile_store(current_profile);
            ESP_LOGI(TAG, "current_profile=%s", profile_names[current_profile]);

            char reply[48];
            snprintf(reply, sizeof(reply), "STATUS:profile=%s", profile_names[current_profile]);
            uplink_send_control(reply);
            return;
        }
    }

    // Deliberately no fallback. A silently-defaulted profile means the wrong
    // blind keystrokes later, which is worse than a rejected command now.
    ESP_LOGW(TAG, "PROFILE: rejected unknown value '%s' (profile unchanged: %s)",
             value, profile_names[current_profile]);

    char reply[80];
    snprintf(reply, sizeof(reply), "ERR:profile unknown '%s', still %s",
             value, profile_names[current_profile]);
    uplink_send_control(reply);
}

static void handle_type(const char *value) {
    // BLOCKING enqueue: when the emitter queue is full we wait for it to drain
    // rather than dropping characters. This is the near end of the Stage 3 flow
    // control chain -- a full queue stalls the bridge task, which stops draining
    // the uplink RX buffer, which makes link_core_deliver_framed reject further
    // radio bytes, which withholds the ARQ ACK, which makes the sender pause.
    // The emitter always drains (it skips, never blocks, when USB is unmounted),
    // so this can never deadlock. A silent drop here would defeat the whole
    // reliability point of ARQ, so we never drop.
    size_t queued = 0;
    for (const char *p = value; *p != '\0'; p++) {
        hid_event_t ev = {.kind = HID_EVT_CHAR, .mod = 0, .code = (uint8_t)*p};
        xQueueSend(s_hid_queue, &ev, portMAX_DELAY);
        queued++;
    }
    ESP_LOGI(TAG, "TYPE: queued %u chars", (unsigned)queued);
}

// HIDTUNE:press,gap -- set the emitter's press-hold and inter-key gap (ms) at
// runtime, for sweeping the reliable-speed floor without a reflash. Clamped to
// a sane range so a typo can't wedge the emitter.
static void handle_hidtune(const char *value) {
    int press = -1, gap = -1;
    if (sscanf(value, "%d,%d", &press, &gap) != 2 || press < 1 || gap < 1
        || press > 100 || gap > 100) {
        uplink_send_control("ERR:hidtune expects press,gap (each 1-100 ms)");
        return;
    }
    s_hid_press_ms = (uint32_t)press;
    s_hid_gap_ms = (uint32_t)gap;
    char reply[48];
    snprintf(reply, sizeof(reply), "STATUS:hidtune press=%d gap=%d", press, gap);
    uplink_send_control(reply);
}

// KEY:<mod>,<kc>[,<kc>...] -- tap a chord of up to MAX_KEYCODES keys with a
// modifier, then release. Bytes are hex; the host resolves names (enter, cmd,
// c, ...) to the mod bitmask and HID keycodes, so the firmware stays a dumb
// emitter. e.g. "00,28" = Enter; "08,06" = Cmd+c. Reliable: goes through the
// completion-gated, mutex-serialized emit_report, and the release is retried.
static void handle_key(const char *value) {
    uint8_t bytes[1 + MAX_KEYCODES];
    int n = 0;
    const char *p = value;
    while (*p != '\0' && n < (int)sizeof(bytes)) {
        char *end;
        long v = strtol(p, &end, 16);
        if (end == p) {
            break;
        }
        bytes[n++] = (uint8_t)v;
        p = end;
        if (*p == ',') {
            p++;
        }
    }
    if (n < 2) {
        uplink_send_control("ERR:key expects mod,kc[,kc...] (hex)");
        return;
    }

    // Enqueue each keycode as an ordered tap event, so a KEY: after a TYPE:
    // lands after the typed text instead of interleaving. (key/combo send a
    // single keycode; a multi-keycode chord arrives as separate taps.)
    uint8_t mod = bytes[0];
    for (int i = 1; i < n; i++) {
        hid_event_t ev = {.kind = HID_EVT_KEYTAP, .mod = mod, .code = bytes[i]};
        xQueueSend(s_hid_queue, &ev, portMAX_DELAY);
    }
}

// KEYHOLD:<mod>,<kc> -- press a key and leave it down (single held key). KEYUP
// releases all. Ordered through the emitter queue like taps. For key-repeat and
// single-key holds (games); simultaneous multi-key holds are not modelled.
static void handle_keyhold(const char *value) {
    int mod = -1, kc = -1;
    if (sscanf(value, "%x,%x", &mod, &kc) != 2 || mod < 0 || kc < 0 || kc > 0xFF) {
        uplink_send_control("ERR:keyhold expects mod,kc (hex)");
        return;
    }
    hid_event_t ev = {.kind = HID_EVT_HOLD, .mod = (uint8_t)mod, .code = (uint8_t)kc};
    xQueueSend(s_hid_queue, &ev, portMAX_DELAY);
}

static void handle_keyup(void) {
    hid_event_t ev = {.kind = HID_EVT_RELEASE, .mod = 0, .code = 0};
    xQueueSend(s_hid_queue, &ev, portMAX_DELAY);
}

// STATUS -- reply over the uplink with this board's role and link telemetry.
static void handle_status(void) {
    const uint8_t *m = enow_peer_mac();
    char fp[LINKCRYPT_FP_CHARS + 1];
    linkcrypt_fingerprint(fp, sizeof(fp));
    char reply[192];
    snprintf(reply, sizeof(reply),
             "STATUS:role=%s peer=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d retx=%u rxdrop=%u "
             "key=%s paired=%d authfail=%u",
             role_name(s_role), m[0], m[1], m[2], m[3], m[4], m[5],
             enow_last_rssi(), (unsigned)enow_retransmits(), (unsigned)enow_rx_drops(),
             fp, enow_paired() ? 1 : 0, (unsigned)enow_auth_failures());
    uplink_send_control(reply);
}

// Frame a short control reply straight out of the USB CDC port, bypassing the
// uplink. Provisioning happens while the radio is failed closed, so the uplink
// is exactly what is NOT available; the host is on the other end of USB.
static void cdc_reply(const char *msg) {
    static uint8_t frame[256 + FRAME_OVERHEAD];
    uint16_t len = (uint16_t)strlen(msg);
    if (len > 256) {
        len = 256;
    }
    uint16_t total = frame_encode(FRAME_TYPE_CONTROL, (const uint8_t *)msg, len, frame);
    if (total == 0) {
        return;
    }
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame, total);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static bool parse_hex32(const char *hex, uint8_t *out) {
    for (int i = 0; i < LINKCRYPT_KEY_LEN * 2; i++) {
        char c = hex[i];
        uint8_t nib;
        if (c >= '0' && c <= '9') {
            nib = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nib = (uint8_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nib = (uint8_t)(c - 'A' + 10);
        } else {
            return false;
        }
        if (i % 2 == 0) {
            out[i / 2] = (uint8_t)(nib << 4);
        } else {
            out[i / 2] |= nib;
        }
    }
    return hex[LINKCRYPT_KEY_LEN * 2] == '\0';
}

// LINKKEY:<64 hex chars> | LINKKEY:clear -- provision this board's radio key.
//
// Board-local ONLY. This is deliberately never accepted over the radio: handing
// the key across the link it is meant to secure would defeat the whole point,
// and an attacker able to inject one would simply take the link over. Both
// sticks must be given the SAME key over their own USB ports.
static void apply_link_key(const char *value, char *reply, size_t cap) {
    if (strcmp(value, "clear") == 0) {
        bool ok = linkcrypt_clear_psk();
        enow_repair();
        snprintf(reply, cap, ok ? "STATUS:linkkey=none" : "ERR:could not clear link key");
        return;
    }
    uint8_t key[LINKCRYPT_KEY_LEN];
    if (!parse_hex32(value, key)) {
        snprintf(reply, cap, "ERR:link key must be %d hex chars", LINKCRYPT_KEY_LEN * 2);
        return;
    }
    bool ok = linkcrypt_set_psk(key);
    memset(key, 0, sizeof(key));
    if (!ok) {
        snprintf(reply, cap, "ERR:could not store link key");
        return;
    }
    // The old session derived from the old PSK, so it cannot survive.
    enow_repair();
    char fp[LINKCRYPT_FP_CHARS + 1];
    linkcrypt_fingerprint(fp, sizeof(fp));
    snprintf(reply, cap, "STATUS:linkkey=%s", fp);
}

// ROLE:<nest|pigeon|unset> -- persist this board's role and reboot into it.
// Shared by the Pigeon control grammar and the Nest's board-local config path.
// role_apply does not return (it restarts), so any reply is best-effort.
static void apply_role_by_name(const char *value) {
    board_role_t r;
    if (strcmp(value, "nest") == 0) {
        r = ROLE_NEST;
    } else if (strcmp(value, "pigeon") == 0) {
        r = ROLE_PIGEON;
    } else if (strcmp(value, "unset") == 0) {
        r = ROLE_UNSET;
    } else {
        ESP_LOGW(TAG, "ROLE: bad value '%s'", value);
        return;
    }
    role_apply(r);  // persists to NVS and reboots; does not return
}

static void handle_control_frame(const uint8_t *payload, uint16_t len) {
    // +1 for the terminator the grammar is parsed with.
    char buf[FRAME_MAX_PAYLOAD + 1];
    if (len > FRAME_MAX_PAYLOAD) {
        len = FRAME_MAX_PAYLOAD;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    // Split on the FIRST colon only: everything after it is literal, so a
    // TYPE: payload may itself contain colons.
    char *colon = strchr(buf, ':');
    if (colon == NULL) {
        ESP_LOGW(TAG, "control: no ':' in %.32s", buf);
        uplink_send_control("ERR:malformed, expected KEY:value");
        return;
    }
    *colon = '\0';
    const char *key = buf;
    const char *value = colon + 1;

    if (strcmp(key, "PROFILE") == 0) {
        handle_profile(value);
    } else if (strcmp(key, "TYPE") == 0) {
        handle_type(value);
    } else if (strcmp(key, "HIDTUNE") == 0) {
        handle_hidtune(value);
    } else if (strcmp(key, "KEY") == 0) {
        handle_key(value);
    } else if (strcmp(key, "KEYHOLD") == 0) {
        handle_keyhold(value);
    } else if (strcmp(key, "KEYUP") == 0) {
        handle_keyup();
    } else if (strcmp(key, "STATUS") == 0) {
        handle_status();
    } else if (strcmp(key, "LINKKEY") == 0) {
        // Arrived over the radio -- refuse. See apply_link_key.
        ESP_LOGW(TAG, "LINKKEY over the radio refused; provision over USB");
        uplink_send_control("ERR:LINKKEY is USB-only, never accepted over the radio");
    } else if (strcmp(key, "ROLE") == 0) {
        apply_role_by_name(value);  // does not return
    } else if (strcmp(key, "FLASHMODE") == 0) {
        // handle_control_frame is only ever invoked here via on_frame, which
        // only dispatches frames that arrived over the RADIO (see s_deframer /
        // pump_uplink_rx). Bytes arriving on the Pigeon's own USB port go
        // through pump_cdc_rx instead, which treats them as opaque target
        // serial data and never parses them as control frames -- so the
        // target the Pigeon is plugged into can never put it in flash mode.
        flashmode_reboot();  // does not return (on success)
    } else if (strcmp(key, "ENTER") == 0 || strcmp(key, "DELAY") == 0 || strcmp(key, "MOD") == 0) {
        // Reserved in the grammar, deliberately not implemented yet.
        ESP_LOGW(TAG, "control: '%s' is reserved but not implemented", key);
        uplink_send_control("ERR:verb reserved, not implemented");
    } else {
        ESP_LOGW(TAG, "control: unknown verb '%s'", key);
        uplink_send_control("ERR:unknown verb");
    }
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

static void on_frame(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (type == FRAME_TYPE_CONTROL) {
        handle_control_frame(payload, len);
        return;
    }

    // 0x02: opaque serial bytes bound for the CDC port. Held in a ring buffer
    // rather than written straight through, because the CDC TX FIFO only
    // drains as fast as the host reads it.
    size_t sent = xStreamBufferSend(s_cdc_tx, payload, len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "cdc tx ring full, dropped %u bytes", (unsigned)(len - sent));
    }
}

// ---------------------------------------------------------------------------
// The two pumps. Each does a bounded chunk per call and returns; neither ever
// blocks waiting on the other side to catch up.
// ---------------------------------------------------------------------------

// uplink -> deframe -> dispatch
static void pump_uplink_rx(void) {
    uint8_t buf[UPLINK_RX_CHUNK];
    size_t n = xStreamBufferReceive(s_uplink_rx, buf, sizeof(buf), 0);
    if (n > 0) {
        deframer_feed_buf(&s_deframer, buf, (uint16_t)n);
    }
}

// ring buffer -> CDC, writing only what the FIFO reports as free
static void pump_cdc_tx(void) {
    if (!tud_cdc_connected()) {
        // No reader on the other end. Leave the bytes in the ring rather than
        // writing them into a FIFO nobody is draining.
        return;
    }

    uint32_t avail = tud_cdc_write_available();
    bool wrote = false;

    while (avail > 0) {
        uint8_t tmp[CDC_TX_CHUNK];
        size_t want = avail < sizeof(tmp) ? avail : sizeof(tmp);
        size_t got = xStreamBufferReceive(s_cdc_tx, tmp, want, 0);
        if (got == 0) {
            break;  // ring empty
        }
        size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, tmp, got);
        wrote = true;
        if (queued < got) {
            // Wrapper's buffer filled early. Push the remainder back so it is
            // retried next tick instead of being lost.
            xStreamBufferSend(s_cdc_tx, tmp + queued, got - queued, 0);
            break;
        }
        avail -= got;
    }

    if (wrote) {
        // Zero timeout: hand off what is ready, never spin on the TX FIFO.
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
}

// Board-local commands accepted on this board's own USB port while it is
// unprovisioned. Only LINKKEY and STATUS: everything else belongs on the radio.
static void local_on_frame(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (type != FRAME_TYPE_CONTROL) {
        return;
    }
    char buf[128];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    char reply[192];
    if (strncmp(buf, "LINKKEY:", 8) == 0) {
        apply_link_key(buf + 8, reply, sizeof(reply));
    } else if (strncmp(buf, "LOCALSTATUS", 11) == 0 || strncmp(buf, "STATUS", 6) == 0) {
        char fp[LINKCRYPT_FP_CHARS + 1];
        linkcrypt_fingerprint(fp, sizeof(fp));
        snprintf(reply, sizeof(reply), "STATUS:role=%s key=%s paired=0 unprovisioned=1",
                 role_name(s_role), fp);
    } else {
        snprintf(reply, sizeof(reply), "ERR:unprovisioned; only LINKKEY and STATUS accepted");
    }
    cdc_reply(reply);
}

static deframer_t s_local_deframer;

// CDC -> uplink, coalescing a whole read into one frame
static void pump_cdc_rx(void) {
    uint8_t buf[CDC_RX_CHUNK];
    size_t n = 0;

    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &n) != ESP_OK || n == 0) {
        return;
    }

    // Unprovisioned: the radio is failed closed, so there is no target session
    // to bridge to and these bytes cannot be anything but the host provisioning
    // us. Once a key is stored this branch never runs again and the bridge goes
    // back to being byte-transparent -- which is why a stray "LINKKEY:" in the
    // target's own serial output can never be mistaken for a command.
    if (!linkcrypt_has_psk()) {
        deframer_feed_buf(&s_local_deframer, buf, (uint16_t)n);
        return;
    }

    // One frame per drain, not one per byte: a frame per byte would spend 5
    // bytes of overhead and a whole notification on each character.
    uint16_t max = link_max_payload();
    if (max == 0) {
        return;  // nobody subscribed; drop rather than buffer indefinitely
    }

    size_t off = 0;
    while (off < n) {
        uint16_t chunk = (n - off) > max ? max : (uint16_t)(n - off);
        if (!uplink_send(FRAME_TYPE_DATA, buf + off, chunk)) {
            break;
        }
        off += chunk;
    }
}

static void bridge_task(void *arg) {
    (void)arg;
    bool was_mounted = false;

    for (;;) {
        // A freshly attached host has no idea what this keyboard was doing
        // before, so start it from a known all-keys-up state. Detected by
        // polling rather than tud_mount_cb(), which esp_tinyusb already
        // defines and does not let the application override.
        bool mounted = tud_mounted();
        if (mounted && !was_mounted) {
            s_hid_abort = true;
            xQueueReset(s_hid_queue);
            ESP_LOGI(TAG, "USB mounted; releasing keys");
        }
        was_mounted = mounted;

        pump_uplink_rx();
        pump_cdc_tx();
        pump_cdc_rx();
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_TICK_MS));
    }
}

// ---------------------------------------------------------------------------
// Legacy opcode path
// ---------------------------------------------------------------------------

static void handle_command(const uint8_t *data, uint16_t len) {
    if (len < 1) {
        return;
    }

    switch (data[0]) {
        case CMD_KEY_TAP:
            if (len >= 2) {
                tap_keys(data[1], &data[2], (uint8_t)(len - 2));
            }
            break;
        case CMD_KEY_HOLD:
            if (len >= 2) {
                send_keyboard_report(data[1], &data[2], (uint8_t)(len - 2));
            }
            break;
        case CMD_KEY_RELEASE:
            send_keyboard_report(0, NULL, 0);
            break;
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%02X", data[0]);
            break;
    }
}

// ---------------------------------------------------------------------------
// Core side of the transport seam (link.h)
// ---------------------------------------------------------------------------

// Push received framed bytes into the deframer's input buffer. Returns false on
// overflow so the transport can reset any partial-frame tracking it keeps.
bool link_core_deliver_framed(const uint8_t *data, uint16_t len) {
    // ATOMIC accept: enqueue the whole packet or none. A byte-stream partial
    // send would leave a fragment in the buffer; the ARQ then retransmits the
    // whole packet, the fragment gets duplicated ahead of it, and the deframer
    // desyncs -- corrupting a frame (observed as a full line dropped under a
    // burst). Rejecting whole makes the sender retransmit intact once there is
    // room, and the ordered ARQ guarantees no reordering across the reject.
    if (xStreamBufferSpacesAvailable(s_uplink_rx) < len) {
        return false;  // no room for the whole packet; withhold ACK (backpressure)
    }
    size_t sent = xStreamBufferSend(s_uplink_rx, data, len, 0);
    return sent == len;
}

// Run a bare legacy keystroke opcode. Only the BLE daemon path produces these.
void link_core_legacy_cmd(const uint8_t *data, uint16_t len) {
    handle_command(data, len);
}

// Controller/peer lost: drop queued typing and lift all keys, so a dropped link
// never leaves a key held down on the target.
void link_core_controller_lost(void) {
    s_hid_abort = true;
    xQueueReset(s_hid_queue);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

// Fires on the TinyUSB task once an IN report has been collected by the host.
// The emitter waits on this to pace itself at the true host poll rate instead
// of guessing with delays. Overrides TinyUSB's weak stub. Signalling a report
// nobody is waiting for is harmless: emit_report drains a stale count before
// each send, and the binary semaphore saturates at one.
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance;
    (void)report;
    (void)len;
    if (s_hid_report_done != NULL) {
        xSemaphoreGive(s_hid_report_done);
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_descriptor;
}

// ---------------------------------------------------------------------------
// Nest role: Mac-side CDC <-> radio relay + board-local config
// ---------------------------------------------------------------------------

// The Nest deframes its CDC input so it can pick board-local ROLE: commands out
// of the stream and handle them itself, relaying everything else to the Pigeon.
static deframer_t s_nest_deframer;

// Re-encode a frame and push it to the peer over the radio, sliced to the ESP-NOW
// cap. The Pigeon's deframer reassembles across packets, so slicing is safe.
static void nest_relay(uint8_t type, const uint8_t *payload, uint16_t len) {
    if (!enow_paired()) {
        ESP_LOGW(TAG, "relay dropped: no peer paired yet");
        return;
    }
    static uint8_t frame[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];
    uint16_t total = frame_encode(type, payload, len, frame);
    if (total == 0) {
        return;
    }
    for (uint16_t off = 0; off < total; off += ENOW_MAX_PAYLOAD) {
        uint16_t chunk = (total - off) > ENOW_MAX_PAYLOAD ? ENOW_MAX_PAYLOAD : (uint16_t)(total - off);
        for (int t = 0; t < 5 && !enow_send(frame + off, chunk); t++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// One reassembled frame from the Mac. A board-local ROLE: command is handled
// here and never relayed (else it would reassign the Pigeon); everything else
// relays to the target -- but only once this board actually is the Nest.
static void nest_on_frame(uint8_t type, const uint8_t *payload, uint16_t len) {
    // ROLE:<r> -- set THIS board's role (handled locally, never relayed).
    if (type == FRAME_TYPE_CONTROL && len >= 5 && memcmp(payload, "ROLE:", 5) == 0) {
        char val[16];
        uint16_t vlen = len - 5;
        if (vlen >= sizeof(val)) {
            vlen = sizeof(val) - 1;
        }
        memcpy(val, payload + 5, vlen);
        val[vlen] = '\0';
        apply_role_by_name(val);  // does not return
        return;
    }
    // LINKKEY:<hex|clear> -- set THIS board's radio key. Handled locally and
    // never relayed: the Pigeon is provisioned over its own USB port, because
    // shipping the key over the radio would defeat what it is protecting.
    if (type == FRAME_TYPE_CONTROL && len >= 8 && memcmp(payload, "LINKKEY:", 8) == 0) {
        char val[80];
        uint16_t vlen = len - 8;
        if (vlen >= sizeof(val)) {
            vlen = sizeof(val) - 1;
        }
        memcpy(val, payload + 8, vlen);
        val[vlen] = '\0';
        char reply[192];
        apply_link_key(val, reply, sizeof(reply));
        cdc_reply(reply);
        return;
    }
    // LOCALSTATUS -- report THIS board rather than relaying to the Pigeon, so
    // both sticks' key fingerprints can be compared before blaming the radio.
    if (type == FRAME_TYPE_CONTROL && len >= 11 && memcmp(payload, "LOCALSTATUS", 11) == 0) {
        char fp[LINKCRYPT_FP_CHARS + 1];
        linkcrypt_fingerprint(fp, sizeof(fp));
        char reply[192];
        snprintf(reply, sizeof(reply), "STATUS:local role=%s key=%s paired=%d authfail=%u",
                 role_name(s_role), fp, enow_paired() ? 1 : 0,
                 (unsigned)enow_auth_failures());
        cdc_reply(reply);
        return;
    }
    // FLASHMODE:local -- reboot THIS board (the Nest/UNSET stick on this USB
    // port) into the ROM download bootloader. FLASHMODE:peer -- relay to the
    // paired Pigeon so IT reboots into flash mode instead. Handled locally
    // (never falls through to nest_relay) because "local" must never be
    // relayed -- a board cannot ask itself over the radio to reboot.
    if (type == FRAME_TYPE_CONTROL && len >= 10 && memcmp(payload, "FLASHMODE:", 10) == 0) {
        char val[8];
        uint16_t vlen = len - 10;
        if (vlen >= sizeof(val)) {
            vlen = sizeof(val) - 1;
        }
        memcpy(val, payload + 10, vlen);
        val[vlen] = '\0';
        if (strcmp(val, "peer") == 0) {
            nest_relay(type, payload, len);  // the Pigeon reboots on receipt
            return;
        }
        flashmode_reboot();  // does not return (on success)
        return;
    }
    // RESETPAIR -- clear BOTH boards for a swap. Relay an unset to the Pigeon
    // first (blocks until the ARQ delivers it), then unset ourselves. Doing both
    // means neither board advertises NEST afterward, so the freshly-unset Pigeon
    // won't just re-hear a Nest and flip back to Pigeon.
    if (type == FRAME_TYPE_CONTROL && len >= 10 && memcmp(payload, "RESETPAIR:", 10) == 0) {
        static const char unset_cmd[] = "ROLE:unset";
        nest_relay(FRAME_TYPE_CONTROL, (const uint8_t *)unset_cmd, sizeof(unset_cmd) - 1);
        vTaskDelay(pdMS_TO_TICKS(100));  // let the relay land before we reboot
        role_apply(ROLE_UNSET);  // does not return
    }
    if (s_role != ROLE_NEST) {
        return;  // UNSET has no peer purpose yet; wait for a role
    }
    nest_relay(type, payload, len);
}

// Radio -> host: a payload from the Pigeon (control replies, target serial
// readback) written straight to the CDC port. ACK it regardless of DTR: uplink
// is best-effort console output, and the backpressure that matters is on the
// Pigeon's side.
static bool nest_on_radio_rx(const uint8_t *data, uint16_t len) {
    if (tud_cdc_connected()) {
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return true;
}

// Host -> board: drain the CDC port into the deframer, which dispatches whole
// frames to nest_on_frame (local config or relay).
static void nest_task(void *arg) {
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        size_t n = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
            deframer_feed_buf(&s_nest_deframer, buf, (uint16_t)n);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// A peer advertised its role. Only an UNSET board acts, adopting the complement
// and rebooting into it. Runs in the ESP-NOW receive context; role_apply reboots
// so blocking briefly there is fine.
static void on_peer_role(uint8_t peer_role) {
    if (s_role != ROLE_UNSET) {
        return;  // already assigned; ignore
    }
    if (peer_role == ROLE_NEST) {
        role_apply(ROLE_PIGEON);  // does not return
    } else if (peer_role == ROLE_PIGEON) {
        role_apply(ROLE_NEST);  // does not return
    }
}

static void start_nest(board_role_t role) {
    deframer_init(&s_nest_deframer, nest_on_frame);
    enow_set_peer_role_cb(on_peer_role);
    // As the controller side, the Nest sends keepalives so the Pigeon can tell an
    // idle link from a dead one. An UNSET board does not yet drive a target, so it
    // only listens (for a role) until designated.
    enow_init(nest_on_radio_rx);
    if (role == ROLE_NEST) {
        enow_start_keepalive(200);
    }
    xTaskCreate(nest_task, "nest", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "%s up: USB CDC (PID 0x4006) <-> ESP-NOW relay", role_name(role));
}

static void start_pigeon(void) {
    profile_load();
    ESP_LOGI(TAG, "current_profile=%s", profile_names[current_profile]);

    s_uplink_rx = xStreamBufferCreate(UPLINK_RX_BUF_SZ, 1);
    s_cdc_tx = xStreamBufferCreate(CDC_TX_RING_SZ, 1);
    s_hid_queue = xQueueCreate(HID_QUEUE_LEN, sizeof(hid_event_t));
    s_hid_report_done = xSemaphoreCreateBinary();
    assert(s_uplink_rx != NULL && s_cdc_tx != NULL && s_hid_queue != NULL
           && s_hid_report_done != NULL);

    deframer_init(&s_deframer, on_frame);
    deframer_init(&s_local_deframer, local_on_frame);
    link_init();  // ESP-NOW transport (Pigeon side)

    xTaskCreate(bridge_task, "bridge", 4096, NULL, 5, NULL);
    xTaskCreate(hid_emitter_task, "hid_emit", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Pigeon ready (HID keyboard + CDC bridge)");
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    s_role = role_load();
    ESP_LOGI(TAG, "boot role: %s", role_name(s_role));
    bool as_pigeon = (s_role == ROLE_PIGEON);

    build_usb_serial();

    // USB personality is fixed at enumeration, so pick the descriptor set for
    // this role now: HID+CDC for the Pigeon, CDC-only for the Nest/UNSET.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = as_pigeon ? &usb_device_descriptor : &usb_device_descriptor_cdc;
    tusb_cfg.descriptor.full_speed_config = as_pigeon ? usb_configuration_descriptor : usb_configuration_descriptor_cdc;
    tusb_cfg.descriptor.string = usb_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(usb_string_descriptor) / sizeof(usb_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = as_pigeon ? usb_configuration_descriptor : usb_configuration_descriptor_cdc;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    // Advertise our role to the peer, so an UNSET peer can adopt the complement.
    enow_set_local_role((uint8_t)s_role);

    if (as_pigeon) {
        start_pigeon();
    } else {
        start_nest(s_role);
    }
}
