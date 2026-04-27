#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_partition.h"

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
RTC_NOINIT_ATTR uint32_t s_boot_count;
RTC_NOINIT_ATTR static uint32_t s_consec_sw_resets;
static bool s_skip_i2c_restart = false;
// Safe mode skips heavy I2C peripherals (VL53 sensors) when the previous
// boot ended in a panic. The first VL53 read after a panicked boot tends
// to re-trigger the same i2c_master ISR race and put the board in a death
// loop. Skipping VL53 lets the UI come up so the user can recover.
bool g_safe_mode = false;
// Ultra safe mode (≥3 consecutive panics) skips ALL I2C peripherals and
// the touch-polling clock_task. The board boots to a static message
// screen so the user knows to power-cycle / apply the IDF patch.
bool g_ultra_safe_mode = false;

const char *reset_reason_str(esp_reset_reason_t reason)
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

/* â”€â”€ Interactive CLI Task â”€â”€ */
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

/* I2C bus health watchdog: periodically probes TCA9548A. If it doesn't
   answer for several rounds, it means a slave is holding SDA low and
   the touch screen / sensors / RTC are all dead. Try a runtime
   GPIO unstick first; if that doesn't help, restart cleanly so the
   user doesn't have to physically pull power. */
static void i2c_watchdog_task(void *arg)
{
    (void)arg;
    const uint8_t probe_addr = ADDR_TCA9548A;
    int consecutive_fails = 0;
    int recoveries = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (i2c_manager_ping(probe_addr) == ESP_OK) {
            consecutive_fails = 0;
            continue;
        }
        consecutive_fails++;
        if (consecutive_fails < 3) continue;

        ESP_LOGW(TAG, "I2C watchdog: TCA9548A unreachable %d rounds — recovering",
                 consecutive_fails);
        if (i2c_manager_recover_bus() == ESP_OK &&
            i2c_manager_ping(probe_addr) == ESP_OK) {
            consecutive_fails = 0;
            recoveries++;
            ESP_LOGI(TAG, "I2C watchdog: recovered (total recoveries=%d)", recoveries);
            continue;
        }

        // Do NOT esp_restart on recovery failure: chip reset doesn't power
        // cycle the modules either, so we just bounce right back into the
        // same hung-bus state and the watchdog fires again. The reset
        // loop made things worse — at least staying alive lets the user
        // reach /tech over WiFi.
        ESP_LOGE(TAG, "I2C watchdog: recovery failed — staying alive in degraded mode "
                 "(user must power-cycle modules' VCC)");
        consecutive_fails = 0;
    }
}

/* â”€â”€ SNTP Sync Task â”€â”€ */
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

    // VL53 pill sensors are started in app_main (they don't need Wi-Fi).

    extern bool g_system_ready;
    g_system_ready = true;
    vTaskDelete(NULL);
}

// In safe mode we skip the heavy WiFi/camera/web stack (those are the
// suspected crash source after a panic). Bring up the minimum that lets
// the dispenser still serve scheduled meds: NVS settings, audio for
// med-reminder playback (UART, no WiFi needed), the dispenser
// scheduler (RTC + servo only), and USB. Display exits its boot screen
// via g_system_ready=true so the user sees the home page.
static void safe_mode_init_task(void *arg)
{
    (void)arg;
    dfplayer_init();
    settings_load_nvs();
    dispenser_scheduler_start();
    usb_mouse_start();
    extern bool g_system_ready;
    g_system_ready = true;
    ESP_LOGW(TAG, "Safe mode ready: scheduler+audio+USB+display only "
                  "(WiFi/camera/web/MQTT disabled until power-cycle)");
    vTaskDelete(NULL);
}

void app_main(void)
{
    web_log_init();
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    ESP_LOGI(TAG, "Starting Unified Cam & Modules (ESP32-P4 Nano)");
    esp_reset_reason_t _early_reason = esp_reset_reason();
    // POWERON definitively means the chip lost power — wipe all RTC-NOINIT
    // state regardless of whatever bytes happen to be in s_boot_magic
    // (RTC RAM doesn't reliably zero on power-loss, so the magic check
    // alone is unreliable). Otherwise honour the magic so SW resets keep
    // boot_count + consec_sw_resets across the reboot.
    if (_early_reason == ESP_RST_POWERON || s_boot_magic != BOOT_MAGIC) {
        s_boot_magic = BOOT_MAGIC;
        s_boot_count = 0;
        s_consec_sw_resets = 0;
    }
    s_boot_count++;
    // Count both clean SW restarts (esp_restart from watchdog) and panics
    // — both leave the I2C bus in the same hung state and need a real
    // power-cycle, so neither benefits from another auto-restart.
    if (_early_reason == ESP_RST_SW || _early_reason == ESP_RST_PANIC ||
        _early_reason == ESP_RST_TASK_WDT || _early_reason == ESP_RST_INT_WDT) {
        s_consec_sw_resets++;
    } else {
        s_consec_sw_resets = 0;
    }
    // Stop the I2C watchdog from looping if we've already done several
    // self-restarts that didn't fix the bus — the user must power-cycle.
    if (s_consec_sw_resets >= 3) {
        s_skip_i2c_restart = true;
        g_ultra_safe_mode = true;
    }
    if (s_consec_sw_resets >= 1 && _early_reason == ESP_RST_PANIC) {
        g_safe_mode = true;
    }
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGW(TAG, "Boot #%lu, reset reason: %s (%d)%s",
             (unsigned long)s_boot_count, reset_reason_str(reason), (int)reason,
             g_ultra_safe_mode ? "  [ULTRA SAFE — all I2C off]"
             : g_safe_mode ? "  [SAFE MODE — VL53 disabled]" : "");

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
    if (g_ultra_safe_mode) {
        ESP_LOGW(TAG, "Ultra safe mode: skipping I2C bus init entirely "
                      "(>=3 consecutive panics — please apply i2c_master "
                      "NULL guard patch and rebuild)");
    } else {
        ret = i2c_manager_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Continuing without I2C peripherals");
        } else {
            i2c_ready = true;
        }
    }
    if (i2c_ready) {

// VL53 bootstrap moved to deferred_init_task to avoid double-init

    // Bus scan removed: probing 117 addresses leaves stale state in the
    // ESP-IDF v5.3 i2c_master driver that races with the first VL53
    // transactions and panics i2c_isr_receive_handler at ptr=NULL.
    // Per-device pings below are enough to know what's connected.
    ESP_LOGI(TAG, "Skipping bus scan (per-device pings only)");

#if ENABLE_VL53_PILL_SENSORS
    if (tca9548a_init() != ESP_OK) {
        ESP_LOGW(TAG, "TCA9548A not found at 0x%02X — VL53 will be skipped", ADDR_TCA9548A);
    }
#endif

    // 3. Initialize I2C Devices
    // PCA9685 (Servo)
    if (i2c_manager_ping(ADDR_PCA9685) == ESP_OK) {
        pca9685_init();
    } else {
        ESP_LOGW(TAG, "PCA9685 not found at 0x%02X", ADDR_PCA9685);
    }

    // PCF8574 (IR) â€” set all pins HIGH (input mode) immediately at boot
    if (i2c_manager_ping(ADDR_PCF8574) == ESP_OK) {
        pcf8574_set_all_input();
        ESP_LOGI(TAG, "PCF8574 initialized â€” all pins set to INPUT (0xFF)");
    } else {
        ESP_LOGW(TAG, "PCF8574 not found at 0x%02X", ADDR_PCF8574);
    }

    // FT6336U (Touch screen)
    ft6336u_init();

#if ENABLE_VL53_PILL_SENSORS
    // VL53 init must not sit in deferred_init_task — that path is gated on
    // wifi_sta_init() which blocks waiting for the ESP-Hosted slave chip.
    // Sensors only need I2C + TCA, both ready here.
    if (g_safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping VL53 sensor init "
                      "(previous boot ended in PANIC)");
    } else if (tca9548a_is_present()) {
        vl53l0x_load_calibration_from_nvs();
        vl53l0x_multi_bootstrap();
        vl53l0x_multi_start();
    }
#endif

    }
    // Display hardware init first, then start the periodic UI task immediately.
    display_clock_init();
    if (g_ultra_safe_mode) {
        display_clock_show_ultra_safe();
        ESP_LOGW(TAG, "Ultra safe mode: clock_task NOT started — "
                      "static message on screen, no touch polling");
    } else {
        display_clock_start_task();
    }
#if ENABLE_SD_CARD
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available");
    }
#else
    ESP_LOGI(TAG, "SD card init disabled");
#endif
    // Erase coredump partition every boot we see one — repeatedly
    // re-reading the same dump on each boot consumes flash IO and may
    // be contributing to the bootloop. We've already grabbed what we
    // need from the boot logs.
    {
        const esp_partition_t *cd = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (cd) {
            esp_partition_erase_range(cd, 0, cd->size);
            ESP_LOGW(TAG, "Coredump partition erased (post-panic cleanup)");
        }
    }

    if (g_ultra_safe_mode) {
        ESP_LOGW(TAG, "Ultra safe mode: skipping all init tasks");
    } else if (g_safe_mode) {
        if (xTaskCreate(safe_mode_init_task, "safe_init", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create safe_mode_init task");
        }
    } else {
        if (xTaskCreate(deferred_init_task, "deferred_init", 8192, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create deferred_init task");
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    if (xTaskCreate(i2c_watchdog_task, "i2c_wdog", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create i2c watchdog task");
    }

    // 11. Main loop — periodic heap/uptime log to catch leaks at a glance.
    TickType_t boot_ticks = xTaskGetTickCount();
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        uint32_t uptime_s = (xTaskGetTickCount() - boot_ticks) / configTICK_RATE_HZ;
        unsigned heap_free = (unsigned)esp_get_free_heap_size();
        unsigned heap_min  = (unsigned)esp_get_minimum_free_heap_size();
        ESP_LOGI(TAG, "alive #%lu: uptime=%lus heap_free=%u min_free=%u reset_reason=%s%s",
                 (unsigned long)(++tick),
                 (unsigned long)uptime_s,
                 heap_free, heap_min, reset_reason_str(reason),
                 g_safe_mode ? " [SAFE]" : "");
        // After 2 minutes of stable runtime, clear the consecutive-panic
        // counter so the next reboot tries full mode again. Without this,
        // a single panic would force safe mode forever (until power loss).
        if (uptime_s >= 120 && s_consec_sw_resets != 0) {
            ESP_LOGI(TAG, "Stable for %lus — clearing consec_sw_resets",
                     (unsigned long)uptime_s);
            s_consec_sw_resets = 0;
        }
        // Preemptive reboot if heap critically low — cleaner than random crash later.
        if (heap_free < 16384) {
            ESP_LOGE(TAG, "Heap critically low (%u bytes) — restarting to recover",
                     heap_free);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}
