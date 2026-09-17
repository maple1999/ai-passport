idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_ppt_remote.c"
         "ppt_hid.c"
         "demo_low_power.c"
         "demo_sound_meter.c"
         "sound_meter_model.c"
         "demo_asr.c"
         "asr_audio.c"
         "font_source_han_sans_sc_14_gb2312.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_driver_usb_serial_jtag esp_event esp_hid esp_http_client
             esp_hw_support esp_netif esp_partition esp_timer esp-tls esp_wifi
             json mbedtls nvs_flash
)
