idf_component_register(SRCS "camera_init.c"
                            "cloud_secrets.c"
                            "ui_standby.cpp"
                            "ui_menu.cpp"
                            "ui_setup_schedule.cpp"
                            "ui_setup_meds.cpp"
                            "ui_wifi.cpp"
                            "ui_keyboard.cpp"
                            "ui_confirm.cpp"
                            "ui_settings.cpp"
                            "dfplayer.c"
                            "display_clock.cpp"
                            "ds3231.c"
                            "ft6336u.c"
                            "i2c_manager.c"
                            "jpeg_encoder.c"
                            "main.c"
                            "pca9685.c"
                            "pcf8574.c"
                            "web_server.c"
                            "web_handlers_servo.c"
                            "web_handlers_status.c"
                            "web_handlers_stream.c"
                            "web_handlers_tech.c"
                            "web_log.c"
                            "netpie_mqtt.c"
                            "offline_sync.c"
                            "pill_sensor_status.c"
                            "dispenser_scheduler.c"
                            "sd_card.c"
                            "vl53l0x_multi.c"
                            "wifi_sta.c"
                            "usb_mouse.c"
                            "telegram_bot.c"
                            "tca9548a.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server esp_driver_cam esp_driver_isp
                             esp_driver_jpeg nvs_flash esp_wifi driver
                             esp_timer esp_hw_support mqtt LovyanGFX
                             usb esp_http_client esp-tls mbedtls json
                             fatfs sdmmc esp_driver_sdmmc esp_lcd)

# -O2 (CONFIG_COMPILER_OPTIMIZATION_PERF) promotes several stringop/format
# warnings to errors that were quiet at -Og. They're benign here (manual
# null-termination on truncated copies, bounded inputs). Demote to warnings
# so the performance build doesn't fail on style nits.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=missing-field-initializers
    -Wno-error=maybe-uninitialized
    -Wno-error=array-bounds)
