# One unified firmware image for both sticks. The board reads its role from NVS
# at boot (role.c) and picks its USB personality accordingly (main.c): HID+CDC
# for the Pigeon, CDC-only for the Nest/UNSET. There is no build-time role flag
# any more -- flash this same image to both boards and designate one as the Nest
# with `pigeon-send set-nest`; the other auto-assigns as the Pigeon.
#
# main.c is built around the ESP-NOW link + role negotiation.
idf_component_register(
    SRCS "main.c" "frame.c" "keymap.c" "link_espnow.c" "enow.c" "role.c" "linkcrypt.c" "replay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_tinyusb esp_wifi esp_netif esp_event esp_timer nvs_flash mbedtls
    # Low-level USB/PHY, GPIO and ROM registers used by the flashmode reboot in
    # main.c (usb_switch_to_cdc_jtag) to hand USB to the ROM download bootloader.
    PRIV_REQUIRES esp_hal_clock esp_hal_usb driver esp_rom
)
