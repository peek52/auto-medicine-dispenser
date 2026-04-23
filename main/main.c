#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"

#include "config.h"
#include "i2c_manager.h"
#include "camera_init.h"
#include "pca9685.h" // Moved here as per instruction
#include "wifi_sta.h"
#include "web_server.h"
#include "pcf8574.h"
#include "ft6336u.h"
#include "display_clock.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "ds3231.h"
#include "usb_mouse.h"
#include "esp_sntp.h"
#include "camera_init.h"
#include <time.h>
#include <string.h>
#include "telegram_bot.h"
#include "dfplayer.h"
#include "ui_core.h"  // for settings_load_nvs()
#include "cloud_secrets.h"
#include "offline_sync.h"
#include "vl53l0x_multi.h"
#include "tca9548a.h"
#include "web_log.h"
#if ENABLE_SD_CARD
#include "sd_card.h"
#endif

static const char *TAG = "unified_cam";
static void deferred_init_task(void *arg);
static const uint32_t BOOT_MAGIC = 0xC0D62026u;
RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_boot_count;

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "OTHER";
    }
}

/* ── Interactive CLI Task ── */
static void cli_task(void *arg) {
    char line[128];
    int pos = 0;
    int monitor_mode = 0; // 0=none, 1=time, 2=netpie
    TickType_t last_update = 0;

    while (1) {
        int c = fgetc(stdin);
        
        // --- Monitor Mode ---
        if (monitor_mode != 0) {
            if (c != EOF) {
                if (c == 'q' || c == 'Q' || c == '\x03') { // 'q' or Ctrl+C exits instantly
                    monitor_mode = 0;
                    pos = 0;
                    printf("\n\n[Exited monitor mode]\n");
                }
                continue; // Process keys instantly
            }
            
            // Periodically update monitor info when idle (every 1 sec)
            if (xTaskGetTickCount() - last_update >= pdMS_TO_TICKS(1000)) {
                last_update = xTaskGetTickCount();
                if (monitor_mode == 1) {
                    char tbuf[16], dbuf[32];
                    ds3231_get_time_str(tbuf, sizeof(tbuf));
                    ds3231_get_date_str(dbuf, sizeof(dbuf));
                    printf("\rRTC Time: %s  Date: %s    (Press 'q' to exit)", tbuf, dbuf);
                    fflush(stdout);
                } else if (monitor_mode == 2) {
                    printf("\033[2J\033[H"); // Clear screen
                    printf("--- NETPIE Status Monitor (Press 'q' to exit) ---\n");
                    printf("Connected: %s\n", netpie_is_connected() ? "YES" : "NO");
                    const netpie_shadow_t *shadow = netpie_get_shadow();
                    if (shadow->loaded) {
                        printf("Schedule Enabled: %d\n", shadow->enabled);
                        
                        // Show Slot Times
                        printf("Times: M.Pre[%s] M.Post[%s] N.Pre[%s] N.Post[%s] E.Pre[%s] E.Post[%s] Bed[%s]\n",
                            shadow->slot_time[0], shadow->slot_time[1], shadow->slot_time[2],
                            shadow->slot_time[3], shadow->slot_time[4], shadow->slot_time[5],
                            shadow->slot_time[6]);
                        printf("--------------------------------------------------------------------------------------\n");

                        // Also show real PCA9685 angles
                        extern pca9685_servo_cfg_t g_servo[PCA9685_NUM_CHANNELS]; // Access configuration
                        for (int i=0; i<6; i++) {
                            if (shadow->med[i].name[0]) {
                                printf("Med %d [%-8s]: %2d pills, slots=0x%02X | Servo = Home: %3d, Work: %3d\n", 
                                    i+1, shadow->med[i].name, shadow->med[i].count, shadow->med[i].slots,
                                    g_servo[i].home_angle, g_servo[i].work_angle);
                            }
                        }
                    } else {
                        printf("Shadow Data: NOT LOADED YET\n");
                    }
                    printf("-------------------------------------------------\n");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // Stay responsive!
            continue;
        }

        // --- Normal Mode ---
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Echo character back to terminal (optional, good for visibility)
        if (c != '\n' && c != '\r') {
            fputc(c, stdout);
            fflush(stdout);
        }

        // Handle Backspace
        if (c == '\b' || c == 0x7f) {
            if (pos > 0) {
                pos--;
                printf("\b \b"); // Erase character from terminal
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            // Echo newline
            if (pos > 0 || c == '\n') printf("\n");
            
            line[pos] = '\0';
            if (pos == 0) continue; // skip empty lines

            // --- Process Command ---
            if (strcmp(line, "admin16") == 0 || strcmp(line, "?") == 0 || strcmp(line, "help") == 0) {
                printf("\n--- System Commands ---\n");
                printf("  logcam on   : Enable camera frame logs\n");
                printf("  logcam off  : Disable camera frame logs\n");
                printf("  netpie      : LIVE Monitor NETPIE Status\n");
                printf("  time        : LIVE Monitor RTC Time\n");
                printf("-----------------------\n\n");
            } 
            else if (strcmp(line, "logcam off") == 0) {
                camera_toggle_log(false);
                printf("[Camera Logs Disabled]\n");
            }
            else if (strcmp(line, "logcam on") == 0) {
                camera_toggle_log(true);
                printf("[Camera Logs Enabled]\n");
            }
            else if (strcmp(line, "netpie") == 0) {
                monitor_mode = 2; // Enter netpie monitor mode
            }
            else if (strcmp(line, "time") == 0) {
                monitor_mode = 1; // Enter time monitor mode
                printf("\n");
            }
            else {
                printf("Unknown command: '%s'. Type 'admin16' for help.\n", line);
            }

            // Reset buffer
            pos = 0;
        } else {
            // Add to buffer if valid ASCII and has space
            if (pos < sizeof(line) - 1 && c >= 32 && c <= 126) {
                line[pos++] = (char)c;
            }
        }
    }
}

/* ── SNTP Sync Task ── */
static void sync_time_task(void *arg) {
    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    setenv("TZ", "ICT-7", 1);
    tzset();

    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 30) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/30)", retry);
    }

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year >= (2024 - 1900)) {
        ESP_LOGI(TAG, "Time synced from NTP: %s", asctime(&timeinfo));
        ds3231_set_time(&timeinfo);
        ESP_LOGI(TAG, "RTC DS3231 updated from NTP.");

        // Start Telegram polling after time sync so bot commands work normally.
        telegram_init();
    } else {
        ESP_LOGW(TAG, "Time not synced from NTP properly.");
    }

    vTaskDelete(NULL);
}

static void deferred_init_task(void *arg)
{
    (void)arg;

    // Initialize audio/settings in the background. Audio is currently disabled in firmware.
    dfplayer_init();
    settings_load_nvs();
    wifi_sta_init();
    // VL53 pill sensors removed per user request to fix pin conflict with DFPlayer
    if (xTaskCreate(sync_time_task, "sync_time", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sync_time task");
    }
    netpie_init();
    camera_init();
    start_webserver();
    start_stream_server();
    dispenser_scheduler_start();
    display_clock_set_ip(wifi_sta_get_ip());
    usb_mouse_start();
    if (xTaskCreate(cli_task, "cli_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cli_task");
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    web_log_init();
    ESP_LOGI(TAG, "Starting Unified Cam & Modules (ESP32-P4 Nano)");
    if (s_boot_magic != BOOT_MAGIC) {
        s_boot_magic = BOOT_MAGIC;
        s_boot_count = 0;
    }
    s_boot_count++;
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGW(TAG, "Boot #%lu, reset reason: %s (%d)",
             (unsigned long)s_boot_count, reset_reason_str(reason), (int)reason);

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase_ret = nvs_flash_erase();
        if (erase_ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(erase_ret));
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing with degraded mode (NVS features may be unavailable)");
    }
    cloud_secrets_init();
    offline_sync_init();

    // 2. Initialize Shared I2C Bus (GPIO7/8, 50kHz)
    bool i2c_ready = false;
    ret = i2c_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without I2C peripherals");
    } else {
        i2c_ready = true;
    }
    if (i2c_ready) {

#if ENABLE_VL53_PILL_SENSORS
    // init TCA9548A multiplexer ก่อน VL53 bootstrap
    if (tca9548a_init() == ESP_OK) {
        vl53l0x_multi_bootstrap();
        vl53l0x_multi_start();
    } else {
        ESP_LOGW(TAG, "TCA9548A not found — VL53 skipped");
    }
#endif

    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t a = 3; a < 0x78; a++) {
        if (i2c_manager_ping(a) == ESP_OK) {
            ESP_LOGI(TAG, " -> Found device at 0x%02X", a);
        }
    }
    ESP_LOGI(TAG, "Scan complete.");

    // 3. Initialize I2C Devices
    // PCA9685 (Servo)
    if (i2c_manager_ping(ADDR_PCA9685) == ESP_OK) {
        pca9685_init();
    } else {
        ESP_LOGW(TAG, "PCA9685 not found at 0x%02X", ADDR_PCA9685);
    }

    // PCF8574 (IR) — set all pins HIGH (input mode) immediately at boot
    if (i2c_manager_ping(ADDR_PCF8574) == ESP_OK) {
        pcf8574_set_all_input();
        ESP_LOGI(TAG, "PCF8574 initialized — all pins set to INPUT (0xFF)");
    } else {
        ESP_LOGW(TAG, "PCF8574 not found at 0x%02X", ADDR_PCF8574);
    }

    // FT6336U (Touch screen)
    ft6336u_init();

    }
    // Display hardware init first, but keep the periodic UI task stopped until after VL53 bootstrap.
    display_clock_init();
    display_clock_start_task();
#if ENABLE_SD_CARD
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available");
    }
#else
    ESP_LOGI(TAG, "SD card init disabled");
#endif
    if (xTaskCreate(deferred_init_task, "deferred_init", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create deferred_init task");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 11. Main loop (idle, FreeRTOS handles everything else)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Minimal idle logic (remove heavy logging)
    }
}
