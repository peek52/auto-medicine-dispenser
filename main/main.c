#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"

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
#include "driver/gpio.h"
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
// safe_mode / ultra_safe_mode removed 2026-05-10. The user only runs
// online + offline modes; both go through the full deferred_init_task
// path, so the legacy panic-recovery branches were dead code adding
// noise. If the board panics, it just panics — the upstream IDF
// i2c_master patch + targeted i2c_wdog/late_detect/VL53 gating already
// prevent the known panic causes.

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
/* Scheduler liveness canary — see app_main for rationale. Subscribes to
 * TWDT (configured 30 s in sdkconfig). If the FreeRTOS scheduler stalls
 * because every higher-priority task is blocked on VFS lock contention,
 * this canary stops feeding the watchdog → ESP-IDF panic handler reboots
 * the device → user gets back to a working screen automatically. Logs
 * via esp_rom_printf so even with VFS jammed we still see the tick. */
void liveness_canary_task(void *arg)
{
    (void)arg;
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGE("liveness", "TWDT subscribe failed");
        vTaskDelete(NULL);
        return;
    }
    esp_rom_printf("[LIVENESS] canary armed (TWDT auto-reboot if stuck)\n");
    uint32_t tick = 0;
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
        if ((++tick % 30) == 0) {
            esp_rom_printf("[LIVENESS] tick=%u uptime~%us\n",
                           (unsigned)tick, (unsigned)(tick * 2));
        }
    }
}

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
                printf("  i2cscan     : Scan I2C bus 0x08..0x77 and print all ACK'd addresses\n");
                printf("  wifi clear  : Erase saved Wi-Fi credentials (forces AP setup mode after reboot)\n");
                printf("  reboot      : Restart the device\n");
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
            else if (strcmp(line, "i2cscan") == 0) {
                /* On-demand I²C scan. Useful when swapping camera
                 * modules or peripherals — prints every address that
                 * ACKs in the 0x08..0x77 range. */
                printf("[I2C scan begin]\n");
                int found = 0;
                for (uint8_t a = 0x08; a <= 0x77; ++a) {
                    if (i2c_manager_ping(a) == ESP_OK) {
                        printf("  0x%02X ACK\n", a);
                        found++;
                    }
                }
                printf("[I2C scan end] %d device(s) found\n", found);
                printf("Reference: Cam=0x36, Touch=0x38, PCA=0x40, EEPROM=0x56, RTC=0x68, TCA=0x70, PCF=0x20\n");
            }
            else if (strcmp(line, "wifi clear") == 0) {
                /* Erase only the wifi_cfg NVS namespace. Cloud secrets,
                 * UI settings, VL53 calibration etc. live in other
                 * namespaces and stay intact. After reboot the SSID
                 * falls back to compiled defaults; if those don't
                 * match the user's router the firmware drops into
                 * AP-setup mode (SSID "unified_cam_setup"). */
                nvs_handle_t h;
                esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &h);
                if (err == ESP_OK) {
                    nvs_erase_all(h);
                    nvs_commit(h);
                    nvs_close(h);
                    printf("[Wi-Fi credentials erased] Reboot to apply (type 'reboot' or power-cycle).\n");
                } else {
                    printf("[Wi-Fi clear FAILED] nvs_open(wifi_cfg) -> %s\n", esp_err_to_name(err));
                }
            }
            else if (strcmp(line, "reboot") == 0) {
                printf("[Rebooting...]\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
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
#if !ENABLE_VL53_PILL_SENSORS
    ESP_LOGW(TAG, "I2C watchdog disabled because VL53/TCA support is off");
    vTaskDelete(NULL);
    return;
#endif
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

    // Periodic NTP resync every 6 hours so the DS3231 doesn't drift out
    // of sync over weeks of uptime. SNTP is already initialised; we just
    // pull the result, push it to the RTC, sleep, repeat.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(6 * 60 * 60 * 1000));  // 6h
        sntp_restart();
        for (int wait = 0; wait < 30; ++wait) {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        time_t now2 = 0;
        struct tm tm2 = {0};
        time(&now2);
        localtime_r(&now2, &tm2);
        if (tm2.tm_year >= (2024 - 1900)) {
            ds3231_set_time(&tm2);
            ESP_LOGI(TAG, "RTC re-synced from NTP: %s", asctime(&tm2));
        }
    }
}

static void deferred_init_task(void *arg)
{
    (void)arg;

    // Brief settle before any ESP_LOG calls — gives the i2c_master
    // driver's interrupt queue time to flush after boot probes that
    // failed against missing modules (IDF v5.3.2 race in
    // i2c_ll_read_rxfifo can otherwise panic this task's first log).
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "deferred_init: starting (uptime ~%lu ms)",
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));

    // 1) Audio + settings (no I/O blocking). DY-HV20T on UART1 GPIO 37.
    ESP_LOGI(TAG, "deferred_init: dfplayer_init");
    dfplayer_init();
    ESP_LOGI(TAG, "deferred_init: settings_load_nvs");
    settings_load_nvs();

    // 2) Camera + web servers BEFORE WiFi. wifi_sta_init() can block
    //    up to 15 s waiting for ESP-Hosted to associate; if camera/web
    //    init were sequenced after that, every flash where WiFi is
    //    slow leaves /tech stream and /photo dead until reboot.
    //    Web servers just open listeners — they happily wait for IP
    //    without needing WiFi up first.
    /* camera_init() now runs early in app_main (before silent deadlock
     * window). Skip the duplicate call here to avoid double-init. */

    // lwIP + default event loop must exist before httpd_start() — otherwise
    // tcpip_send_msg_wait_sem aborts with "Invalid mbox". wifi_sta_init()
    // does these later, but start_webserver runs first by design (so the
    // listener is up while WiFi is still trying to associate). Bring the
    // networking core up here; both calls are idempotent.
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();

    ESP_LOGI(TAG, "deferred_init: start_webserver");
    start_webserver();
    ESP_LOGI(TAG, "deferred_init: start_stream_server");
    start_stream_server();

    // 3) Things that don't need WiFi.
    ESP_LOGI(TAG, "deferred_init: dispenser_scheduler_start");
    dispenser_scheduler_start();
    ESP_LOGI(TAG, "deferred_init: usb_mouse_start");
    usb_mouse_start();

    // 4) Now bring up WiFi. May block ~15 s.
    ESP_LOGI(TAG, "deferred_init: wifi_sta_init (may block up to 15s)");
    wifi_sta_init();
    ESP_LOGI(TAG, "deferred_init: wifi_sta_init done, ip=%s", wifi_sta_get_ip());
    display_clock_set_ip(wifi_sta_get_ip());

    // 5) Network-dependent services.
    if (xTaskCreate(sync_time_task, "sync_time", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sync_time task");
    }
    ESP_LOGI(TAG, "deferred_init: netpie_init");
    netpie_init();

    if (xTaskCreate(cli_task, "cli_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cli_task");
    }

    // VL53 pill sensors are started in app_main (they don't need Wi-Fi).

    ESP_LOGI(TAG, "deferred_init: complete, marking system ready");
    extern bool g_system_ready;
    g_system_ready = true;
    vTaskDelete(NULL);
}

// In safe mode we skip the heavy WiFi/camera/web stack (those are the
// suspected crash source after a panic). Bring up the minimum that lets
// safe_mode_init_task removed 2026-05-10 — see top-of-file comment.

// Late-detect any I2C device that was missing during the boot probe.
// Retries for up to ~3 min and brings devices online once they finally
// ACK. Frees the `missing[3]` buffer when done. Caller passes a malloc'd
// bool[3] = { tca_missing, pca_missing, pcf_missing }.
//
// Also attempts to recover the bus when the driver reports
// ESP_ERR_INVALID_STATE — without this the bus stays wedged for the
// whole detection window and slow-waking modules never come online.
void late_detect_task(void *arg)
{
    bool *missing = (bool *)arg;
    bool tca_left = missing[0];
    bool pca_left = missing[1];
    bool pcf_left = missing[2];

    // Track consecutive wedge rounds so we space out recovery attempts —
    // hammering recover_bus() every 3 s can itself confuse the driver.
    int wedge_streak = 0;

    const int TOTAL_ROUNDS = 60;  // 60 × 3 s = 180 s

    ESP_LOGW(TAG, "Late-detect armed: TCA=%d PCA=%d PCF=%d (window=%ds)",
             tca_left, pca_left, pcf_left, TOTAL_ROUNDS * 3);

    for (int round = 0; round < TOTAL_ROUNDS && (tca_left || pca_left || pcf_left); ++round) {
        vTaskDelay(pdMS_TO_TICKS(3000));

        // Heartbeat every 10 rounds (30s) so the user sees we're still trying.
        if (round > 0 && (round % 10) == 0) {
            ESP_LOGW(TAG, "Late-detect: still searching at +%ds (TCA=%d PCA=%d PCF=%d)",
                     round * 3, tca_left, pca_left, pcf_left);
        }

        // If the i2c driver is wedged (ESP_ERR_INVALID_STATE), continuing
        // to poke it can trigger a store-fault inside i2c_isr_receive_handler.
        // Try to actually recover the bus rather than waiting it out: tear
        // down the master bus, GPIO-unstick SDA, recreate. After that the
        // next round can probe again with a clean driver state.
        esp_err_t probe = i2c_manager_ping(ADDR_TCA9548A);
        if (probe == ESP_ERR_INVALID_STATE) {
            wedge_streak++;
            // First wedge of a streak gets recovery immediately; further
            // wedges back off (every 3rd round) so we don't loop forever.
            if (wedge_streak == 1 || (wedge_streak % 3) == 0) {
                ESP_LOGW(TAG, "Late-detect: bus wedged at round %d — recovering",
                         round + 1);
                i2c_manager_recover_bus();
            } else {
                ESP_LOGW(TAG, "Late-detect: bus still wedged (streak=%d, round %d)",
                         wedge_streak, round + 1);
            }
            continue;
        }
        wedge_streak = 0;

        if (tca_left && probe == ESP_OK) {
            ESP_LOGW(TAG, "Late-detect: TCA9548A appeared at round %d — "
                          "bringing VL53 online", round + 1);
            if (tca9548a_init() == ESP_OK) {
#if ENABLE_VL53_PILL_SENSORS
                vl53l0x_load_calibration_from_nvs();
                /* Bootstrap deferred to first /status — see app_main. */
                vl53l0x_multi_start();
#endif
                tca_left = false;
            }
        }
        if (pca_left && i2c_manager_ping(ADDR_PCA9685) == ESP_OK) {
            ESP_LOGW(TAG, "Late-detect: PCA9685 appeared at round %d", round + 1);
            if (pca9685_init() == ESP_OK) pca_left = false;
        }
        if (pcf_left && i2c_manager_ping(ADDR_PCF8574) == ESP_OK) {
            ESP_LOGW(TAG, "Late-detect: PCF8574 appeared at round %d", round + 1);
            pcf8574_set_all_input();
            pcf_left = false;
        }
    }
    if (tca_left || pca_left || pcf_left) {
        ESP_LOGW(TAG, "Late-detect timeout: TCA=%d PCA=%d PCF=%d still missing",
                 tca_left, pca_left, pcf_left);
    }
    free(missing);
    vTaskDelete(NULL);
}

// Set true once we've torn down the I2C master bus this boot. Used to
// suppress later code paths (i2c_watchdog) that would re-create it
// and re-arm the IDF v5.3.2 ISR race we just escaped.
static bool s_i2c_disabled = false;

// True iff tca9548a_init() succeeded this boot. Set inside the
// `if (i2c_ready)` block in app_main and consulted later by the
// i2c_watchdog spawn decision (the watchdog probes TCA every 15 s and
// crashes on the IDF v5.3.2 i2c_master ISR race when TCA is absent).
static bool g_tca_init_ok = false;

// Touch retry task removed: each cycle's i2c_manager_recover_bus +
// probe was triggering the IDF v5.3.2 ISR race AND occasionally
// blocking long enough to fire TASK_WDT (5 s). When the bus is dead
// at boot, leave it dead until the next reboot — the user can
// power-cycle after fixing wiring instead of fighting a retry loop
// that's itself unstable.

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
    // Safe mode auto-trigger removed per user request — even after a panic
    // we boot the FULL stack (NTP, WiFi, camera, VL53). The IDF i2c_master
    // ISR guard + state-reset retry now catches the original race that
    // motivated safe mode, so demoting features after a panic is no longer
    // necessary. Without safe mode the wall-clock keeps syncing via NTP and
    // the user sees the correct time on the display.
    //
    // Still throttle the I2C watchdog after several SW resets — a wedged
    // bus that survives reboot needs a manual power-cycle to recover, not
    // more self-restart loops.
    if (s_consec_sw_resets >= 3) {
        s_skip_i2c_restart = true;
    }
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
        // Seed system time from the battery-backed RTC NOW so the display
        // doesn't show 1970-01-01 in the window before SNTP completes.
        // ds3231_seed_system_time() rejects implausible reads itself, so
        // it's safe to call even if the RTC is missing or has a flat coin
        // cell — system time just stays at epoch and SNTP later fixes it.
        (void)ds3231_seed_system_time();
    }
    if (i2c_ready) {

// VL53 bootstrap moved to deferred_init_task to avoid double-init

    // Bus scan removed: probing 117 addresses leaves stale state in the
    // ESP-IDF v5.3 i2c_master driver that races with the first VL53
    // transactions and panics i2c_isr_receive_handler at ptr=NULL.
    // Per-device pings below are enough to know what's connected.
    ESP_LOGI(TAG, "Skipping bus scan (per-device pings only)");

    /* Full I²C bus scan at boot — list every address 0x08–0x77 that ACKs.
     * Shows what's physically present on the bus (camera SCCB at 0x36
     * shares the bus with TCA/PCA/PCF/DS3231/Touch). Useful when SCCB
     * read of camera fails: confirms whether OV5647 is at 0x36 or
     * has moved to a different address. Adds ~150 ms to boot. */
    {
        ESP_LOGW(TAG, "I2C scan begin (full bus)...");
        char hits[80] = {0};
        size_t pos = 0;
        for (uint8_t a = 0x08; a <= 0x77; ++a) {
            if (i2c_manager_ping(a) == ESP_OK) {
                pos += snprintf(hits + pos, sizeof(hits) - pos, "0x%02X ", a);
                if (pos >= sizeof(hits) - 6) {
                    ESP_LOGW(TAG, "I2C ACK: %s", hits);
                    pos = 0; hits[0] = '\0';
                }
            }
        }
        if (pos > 0) ESP_LOGW(TAG, "I2C ACK: %s", hits);
        ESP_LOGW(TAG, "I2C scan end. Camera should be 0x36; "
                      "TCA=0x70 PCA=0x40 PCF=0x20 RTC=0x68 Touch=0x38");
    }

    /* (Boot CAM-PROBE removed: it was a debug aid that did 8× PID
     * readbacks via i2c_manager_write+read, and subsequent boots
     * occasionally found the sensor stuck after these probes — most
     * likely because the manual write-then-read pattern leaves the
     * i2c_master state in a way the SCCB driver's transmit_receive
     * doesn't expect. The real camera_init below has its own retry
     * + bus recovery so we don't need a separate diag probe.) */

    /* CAMERA INIT WHILE BUS IS PRISTINE — must run before FT6336U /
     * PCA9685 / PCF8574 / TCA9548A / VL53 init touch the bus, because
     * those leave the IDF i2c_master state machine in a configuration
     * where compound transmit_receive (used by SCCB PID readback)
     * intermittently fails INVALID_STATE. Running camera here while
     * only the boot probes have run gives the cleanest possible bus
     * for the OV5647 SCCB burst, which is the most fragile I/O the
     * firmware ever performs. After this block the camera is in
     * continuous-stream mode and we don't touch the OV5647 again
     * unless the user triggers a manual reinit.
     *
     * If camera_ensure_initialized() returns FAIL (all 16 detect
     * attempts failed — happens ~20 % of cold boots when the OV5647
     * comes up in a stuck state), we spawn a background retry task
     * that wakes every 8 s to call camera_ensure_initialized() until
     * it succeeds. The retry doesn't block the rest of boot. */
    {
        ESP_LOGW(TAG, "CAMERA INIT (early, before any peripheral driver init)");
        extern esp_err_t camera_ensure_initialized(void);
        esp_err_t cr = camera_ensure_initialized();
        ESP_LOGW(TAG, "Early camera init → %s", esp_err_to_name(cr));
        if (cr != ESP_OK) {
            extern void camera_init_background_retry_start(void);
            camera_init_background_retry_start();
        }
    }

    // 2 s settle for modules' 3.3 V rail to come up fully — TCA + VL53
    // breakouts in particular have observed cold-start delays beyond
    // 1 s. Without this margin the very first transaction NACKs even
    // when wiring is correct.
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Release FT6336U reset BEFORE probing — without this the touch
    // chip's RST line floats (default GPIO config) which can hold it
    // permanently in reset, making every subsequent ping NACK even
    // when the chip itself is healthy. Configure CTP_RST high before
    // the pre-probe so FT6336U is actually awake when we ping it.
#if CTP_RST_PIN >= 0
    {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << CTP_RST_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 1,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_cfg);
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
#endif

    // Probe FT6336U FIRST with the SAME retry budget as the medication
    // modules below (4×300ms). A single-shot probe was missing the chip
    // on cold boot when the rail hadn't fully settled, which then
    // gated ft6336u_init() out and left the user with a non-touch
    // screen even though hardware was fine.
    bool touch_alive_pre = false;
    for (int a = 0; a < 4; ++a) {
        if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) { touch_alive_pre = true; break; }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (touch_alive_pre) {
        ESP_LOGI(TAG, "FT6336U responded on initial ping");
    } else {
        ESP_LOGW(TAG, "FT6336U did not ACK initial ping (will retry after module probes)");
    }

#if ENABLE_VL53_PILL_SENSORS
    // Cache the TCA init result. Don't re-ping later — the IDF v5.3.2
    // i2c_master_probe race can return spurious OK from electrical noise,
    // which would gate VL53 bootstrap on garbage and hammer the bus for
    // ~45 s while every channel retries × 5, locking out camera/touch
    // and eventually panicking the chip.
    bool s_tca_init_ok = (tca9548a_init() == ESP_OK);
    g_tca_init_ok = s_tca_init_ok;
    if (!s_tca_init_ok) {
        ESP_LOGW(TAG, "TCA9548A not found at 0x%02X — VL53 will be skipped", ADDR_TCA9548A);
    }
#endif

    // 4 retries × 300 ms = 1.2 s window per device. Upping back from
    // the brief 2-retry window: when modules are present but slow,
    // 2 retries was missing them. The bus-disable mitigation handles
    // the dead-bus case downstream, so we can spend more retry budget
    // here on the live-but-slow case.
    #define I2C_PING_RETRIES 4
    #define I2C_PING_GAP_MS  300

    // PCA9685 (Servo)
    bool pca_ok = false;
    for (int a = 0; a < I2C_PING_RETRIES; ++a) {
        if (i2c_manager_ping(ADDR_PCA9685) == ESP_OK) { pca_ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(I2C_PING_GAP_MS));
    }
    if (pca_ok) {
        pca9685_init();
    } else {
        ESP_LOGW(TAG, "PCA9685 not found at 0x%02X after retries", ADDR_PCA9685);
        // Even without hardware, populate g_servo[] from defaults +
        // NVS so the web UI / dashboard still shows the user's saved
        // home/work angles instead of BSS-zero 0/0.
        pca9685_load_cache_only();
    }

    // PCF8574 (IR) â€” set all pins HIGH (input mode) immediately at boot
    bool pcf_ok = false;
    for (int a = 0; a < I2C_PING_RETRIES; ++a) {
        if (i2c_manager_ping(ADDR_PCF8574) == ESP_OK) { pcf_ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(I2C_PING_GAP_MS));
    }
    if (pcf_ok) {
        pcf8574_set_all_input();
        ESP_LOGI(TAG, "PCF8574 initialized â€” all pins set to INPUT (0xFF)");
    } else {
        ESP_LOGW(TAG, "PCF8574 not found at 0x%02X after retries", ADDR_PCF8574);
    }

    // FT6336U (Touch screen). Run init if either:
    //   - the initial probe ACKed (touch alive even if modules dead), or
    //   - any module ACKed (bus is alive, normal case).
    // Skip only when neither — running ft6336u_init's 3 reads on a
    // wedged bus loads stale ISR state in the IDF v5.3.2 i2c_master
    // driver that panics the chip in the next ESP_LOGI critical section.
    static bool s_touch_ok = false;
    if (touch_alive_pre || pca_ok || pcf_ok) {
        s_touch_ok = (ft6336u_init() == ESP_OK);
    } else {
        // All probes failed but user reports hardware is normal.
        // The bus may have been wedged at boot (previous panic left
        // SDA low). Try one full bus recovery + re-probe touch BEFORE
        // declaring the bus dead — touch is the highest-priority
        // peripheral for the user, and ft6336u_init has its own reset
        // pulse + 3-attempt chip-id check, so it won't loop forever
        // on a truly absent chip.
        ESP_LOGW(TAG, "All initial probes failed — attempting bus recovery before giving up");
        if (i2c_manager_recover_bus() == ESP_OK) {
            for (int a = 0; a < 4; ++a) {
                if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
                    touch_alive_pre = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (touch_alive_pre) {
                ESP_LOGW(TAG, "FT6336U appeared after bus recovery — initialising touch");
                s_touch_ok = (ft6336u_init() == ESP_OK);
            } else {
                ESP_LOGW(TAG, "Skipping FT6336U init — entire bus appears dead");
            }
        } else {
            ESP_LOGW(TAG, "Skipping FT6336U init — bus recovery failed");
        }
    }

    /* Camera init has already been done at the top of app_main while
     * the I2C bus was pristine — see "CAMERA INIT WHILE BUS IS PRISTINE"
     * block far above. We DO NOT re-init here. */

#if ENABLE_VL53_PILL_SENSORS
    // VL53 init must not sit in deferred_init_task — that path is gated on
    // wifi_sta_init() which blocks waiting for the ESP-Hosted slave chip.
    // Sensors only need I2C + TCA, both ready here. Order matters: camera
    // SCCB has already finished above so the bus is now safe for VL53.
    bool tca_was_up_at_boot = false;
    if (s_tca_init_ok) {
        // Use the cached init result, not a fresh ping. See comment at
        // tca9548a_init call above.
        tca_was_up_at_boot = true;
        // VL53 in "empty-detect only" mode (user spec 2026-05-11):
        // bootstrap sensors + poll, but use the readings only to drive
        // is_empty flag for display + warnings. Pill count itself
        // stays controlled by servo cycles / Telegram /set / NETPIE
        // shadow — VL53 readings do NOT override it.
        //
        // Polling interval was bumped from 5 s → 30 s in vl53l0x_multi.c
        // so the bus is only briefly busy once per 30 s, letting touch
        // / RTC / camera SCCB get clean access the rest of the time.
        // Broken Ch0 still gets isolated via tca_disable_all_locked
        // after each read.
        vl53l0x_load_calibration_from_nvs();
        /* VL53 bootstrap (init storm: 6 × ~70 sequential I2C writes ≈
         * 10-15 s) was happening here and choking the bus enough that
         * touch reads were timing out for the first 15 s after boot —
         * user felt the screen was unresponsive. Now deferred until the
         * first /status request; vl53_task does it lazily on its own
         * thread. Boot completes with the bus quiet → touch is snappy
         * the moment the screen draws. */
        vl53l0x_multi_start();
    }
#endif

    // When NOTHING acked, the I2C bus is dead (no power on the rail
    // or no slaves connected). Tear down the i2c_master driver so its
    // ISR can never fire — the IDF v5.3.2 race fires when stale ISR
    // state is touched in the next ESP_LOG critical section, deadlocking
    // the chip permanently. With the driver gone the firmware can boot
    // through to WiFi/web/Telegram so the user can still reach /tech.
    (void)s_touch_ok;
    bool any_present = pca_ok || pcf_ok || s_touch_ok;
    if (!any_present) {
        ESP_LOGW(TAG, "All I2C dead — disabling master bus permanently this boot");
        i2c_manager_disable_bus();
        s_i2c_disabled = true;
    } else {
        // Bug history: missing[0] used to be hardcoded true, which made
        // late_detect re-run TCA9548A + VL53 bootstrap ~30s after boot
        // even when TCA was already initialised at boot. The duplicate
        // init raced with the running VL53 task on the shared I2C bus
        // and panicked the chip mid-dispense (reproducible: "return 2
        // cartridges, board resets"). Skip the late-detect rearm for
        // any peripheral that's already up.
        bool *missing = (bool *)malloc(3 * sizeof(bool));
        if (missing) {
#if ENABLE_VL53_PILL_SENSORS
            // Use the cached "was up at boot" flag instead of re-pinging
            // here. The VL53 init storm just before this point leaves the
            // TCA9548A with a channel still selected and a fresh ping can
            // NACK transiently — which then triggered late_detect_task to
            // re-bootstrap VL53 ~13 s later, monopolising the I2C bus for
            // ~10 s and freezing touch / servo / camera SCCB during that
            // window. If TCA worked at boot we already know it's there.
            missing[0] = !tca_was_up_at_boot;
#else
            missing[0] = false;
#endif
            missing[1] = !pca_ok;
            missing[2] = !pcf_ok;
            // Late-detect disabled: i2c_master_probe() in IDF v5.3.2 has an
            // ISR race that store-faults when probing absent devices
            // (i2c_isr_receive_handler reads from ptr=0x6). Crashes were
            // reproducible whenever TCA was missing — late_detect pinged
            // 0x70 every 3 s and eventually hit the race. If a peripheral
            // is connected after boot, user must reboot to detect it.
            (void)missing[0]; (void)missing[1]; (void)missing[2];
            free(missing);
        }
    }

    }
    // Display hardware init only — defer clock_task until AFTER all
    // other init logs/tasks are spawned. Spawning clock_task here was
    // racing with the next ESP_LOGI in app_main (same-tick log calls
    // from two tasks corrupted UART output and hung the firmware
    // deterministically around the 22 s mark on every cold boot when
    // I2C devices were missing).
    display_clock_init();

    /* Camera is LAZY now — no init at boot. First /capture or /photo
     * request triggers camera_ensure_initialized(). This frees the I2C
     * bus during boot for VL53 bootstrap (otherwise SCCB ~2 s contends
     * with VL53 register init and BOTH fail), and saves init time when
     * the user never asks for a photo. */

#if ENABLE_SD_CARD
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available");
    }
#else
    ESP_LOGI(TAG, "SD card init disabled");
#endif
    // Per-boot coredump erase removed: erase_range from app_main was
    // racing with display SPI/DMA. Manual erase via /tech if needed.
    //
    // Short settle before spawning init tasks. With modules absent the
    // boot probes leave the IDF v5.3.2 i2c_master driver in a fragile
    // state; a brief pause here gives any pending interrupts time to
    // flush before deferred_init_task starts using ESP_LOG aggressively.
    vTaskDelay(pdMS_TO_TICKS(500));

    if (xTaskCreate(deferred_init_task, "deferred_init", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create deferred_init task");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    // i2c_watchdog_task DISABLED UNCONDITIONALLY (2026-05-11). It pings
    // TCA9548A every 15 s and hits the IDF v5.3.2 i2c_master_probe ISR
    // race even when TCA is PRESENT — coredump showed ptr=0xa5a5a5a5
    // (FreeRTOS stack-fill = use-after-free) in i2c_ll_read_rxfifo,
    // triggered by i2c_wdog probing a healthy 0x70. Recovery of a
    // wedged bus is handled adequately by touch's own CTP_RST cycle +
    // i2c_manager_recover_bus on demand; we don't need a separate poll
    // task whose only job is to fire the race on a 15 s clock.
    ESP_LOGW(TAG, "I2C watchdog DISABLED — IDF v5.3.2 probe race risk");
    (void)i2c_watchdog_task;  // keep symbol referenced for the linker
    // NOW it's safe to spawn the display task — all the noisy log calls
    // are done and the other init tasks are running on their own threads.
    display_clock_start_task();
    /* Liveness canary — feeds TWDT every 2 s. If the scheduler stalls
     * (silent UART deadlock from VFS lock contention has been chronic on
     * this firmware), the canary stops feeding and TWDT panic-reboots
     * within 30 s, instead of the screen staying frozen forever until
     * the user power-cycles. Lowest possible priority so any real work
     * still preempts it. */
    extern void liveness_canary_task(void *arg);
    if (xTaskCreate(liveness_canary_task, "liveness", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create liveness canary task");
    }

    /* Spawn the idle-music task last so it's after settings have been
     * loaded from NVS (g_bg_music_enabled / g_bg_music_volume). Plays
     * track 99 in a loop after 1 minute of no touch activity. */
    extern void idle_music_start(void);
    idle_music_start();

    ESP_LOGI(TAG, "app_main: init complete, entering keepalive loop");

    // 11. Main loop — periodic heap/uptime log to catch leaks at a glance.
    TickType_t boot_ticks = xTaskGetTickCount();
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        uint32_t uptime_s = (xTaskGetTickCount() - boot_ticks) / configTICK_RATE_HZ;
        unsigned heap_free = (unsigned)esp_get_free_heap_size();
        unsigned heap_min  = (unsigned)esp_get_minimum_free_heap_size();
        ESP_LOGI(TAG, "alive #%lu: uptime=%lus heap_free=%u min_free=%u reset_reason=%s",
                 (unsigned long)(++tick),
                 (unsigned long)uptime_s,
                 heap_free, heap_min, reset_reason_str(reason));
        // After 2 minutes of stable runtime, clear the consecutive-panic
        // counter so the next reboot tries full mode again. Without this,
        // a single panic would force safe mode forever (until power loss).
        // Wait 5 min instead of 2 before declaring stability — the I2C
        // race typically fires within the first 1–2 minutes if it's
        // going to fire at all, but a 2 min window was sometimes
        // clearing the counter just before a delayed crash.
        if (uptime_s >= 300 && s_consec_sw_resets != 0) {
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
