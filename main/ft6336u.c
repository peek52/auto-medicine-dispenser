#include "ft6336u.h"
#include "config.h"
#include "i2c_manager.h"
#include "pca9685.h"
#include "camera_init.h"   /* g_camera_sccb_in_progress */
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FT6336U";

// Set to true once ft6336u_init() has positively identified the chip.
// If init failed (chip-id NACK or unknown ID), the controller may still
// ACK register reads with garbage; without this guard ft6336u_read_touch()
// would interpret random byte 0 as "1 or 2 touches" and synthesise
// phantom presses (UI moving on its own — observed: "จอกดเอง").
static bool s_touch_initialized = false;

/* Boot-mute timer — first 3 seconds of touch reads are suppressed so
 * the FT6336U fallback mode (chip-id NACK) doesn't fire phantom
 * presses while its register state stabilises. File-static so the
 * recovery path in ft6336u_read_touch() can re-arm it (set back to 0)
 * after a CTP_RST hard-reset. */
static uint32_t s_boot_start_ms = 0;

esp_err_t ft6336u_init(void)
{
    if (CTP_RST_PIN >= 0) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << CTP_RST_PIN);
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 1;
        gpio_config(&io_conf);
        /* Longer reset pulse than the datasheet minimum (5 ms LOW +
         * 200 ms HIGH). Field experience: after an ESP-side panic the
         * FT6336U often stays in a glitched state where the short
         * 50 ms pulse can't dislodge it — the only fix used to be a
         * physical power cycle. Holding RST LOW for 200 ms gives the
         * chip's internal state machine more time to fully drain its
         * registers, then 300 ms HIGH lets it complete its boot
         * sequence before we start I2C transactions. */
        ESP_LOGI(TAG, "Resetting touch controller");
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Try up to 3 times: a recent panic-induced reset can leave the
    // FT6336U glitched (returning garbage like 0x00 from chip-id)
    // even though the I2C transaction succeeds. Retry the GPIO reset
    // pulse and re-read; if all 3 attempts return something other
    // than a known FT ID, treat the controller as unavailable so
    // clock_task doesn't pump bad reads at it.
    static const uint8_t kKnownIds[] = { 0x11, 0x06, 0x64, 0xCD };
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t chip_id = 0;
        if (i2c_manager_read_reg(ADDR_FT6336U, 0xA8, &chip_id, 1) == ESP_OK) {
            for (size_t i = 0; i < sizeof(kKnownIds); ++i) {
                if (chip_id == kKnownIds[i]) {
                    ESP_LOGI(TAG, "Touch controller found. ID: 0x%02X (attempt %d)",
                             chip_id, attempt + 1);
                    s_touch_initialized = true;
                    return ESP_OK;
                }
            }
            ESP_LOGW(TAG, "Touch chip-id 0x%02X not recognised on attempt %d — retrying",
                     chip_id, attempt + 1);
        }
        if (CTP_RST_PIN >= 0) {
            gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
            gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
    // chip-id register (0xA8) didn't return a known value, but the chip
    // may still ACK touch-status register (0x02) reads correctly. This
    // happens after a panic-induced reset where some registers read 0x00.
    // Fall back to using the chip ANYWAY if it ACKs the I2C address —
    // ft6336u_read_touch() applies sanity checks on the touch data
    // (touches count must be 0-2, coords in range) so genuine garbage
    // can't synthesise phantom presses.
    //
    // Multi-attempt ping — the bus may be momentarily wedged from the
    // earlier retry burst. Try 4× with 200 ms gap so a single transient
    // glitch doesn't permanently disable touch (which leaves the screen
    // unresponsive until next reboot). Touch reads have their own
    // periodic re-init via the cheap-probe path in ft6336u_read_touch
    // so even if this still fails the chip can come back online later.
    for (int p = 0; p < 4; ++p) {
        if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
            ESP_LOGW(TAG, "Touch chip-id check failed but I2C address ACKs "
                           "(ping attempt %d) — enabling touch in fallback mode",
                     p + 1);
            s_touch_initialized = true;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* DEEP RECOVERY: 4 ping attempts also failed → chip is in a hard
     * stuck state (common symptom after an ESP-side panic). Try one
     * last hammer: hold RST LOW for a FULL second. This is roughly
     * equivalent to a power-cycle from the chip's internal supply
     * decay perspective — long enough that all internal capacitors
     * discharge and the chip cold-boots fresh on RST release. Costs
     * us 1.5 s of boot time but saves the user from having to unplug
     * the USB cable to recover. */
    if (CTP_RST_PIN >= 0) {
        ESP_LOGW(TAG, "Touch dead after standard reset — attempting DEEP RST (1 s LOW)");
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        for (int p = 0; p < 4; ++p) {
            uint8_t chip_id = 0;
            if (i2c_manager_read_reg(ADDR_FT6336U, 0xA8, &chip_id, 1) == ESP_OK) {
                ESP_LOGW(TAG, "Touch recovered after deep RST: chip_id=0x%02X", chip_id);
                s_touch_initialized = true;
                return ESP_OK;
            }
            if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
                ESP_LOGW(TAG, "Touch ACKed after deep RST (ping %d) — fallback mode", p + 1);
                s_touch_initialized = true;
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    ESP_LOGW(TAG, "Touch controller did not ACK after 4 ping attempts — "
                   "starting in disabled state; ft6336u_read_touch() will "
                   "auto-recover when the chip wakes up");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ft6336u_read_touch(uint16_t *x, uint16_t *y, bool *pressed)
{
    /* All three output pointers must be non-NULL — every internal write
     * site below dereferences them unguarded. The previous mix of "some
     * sites NULL-checked, others not" was a latent crash waiting for a
     * caller that omits one (status_json_handler is the most likely
     * culprit if it ever gets a "touch status" extension). Make NULL a
     * contract violation, fail loudly. */
    if (!x || !y || !pressed) return ESP_ERR_INVALID_ARG;

    static uint32_t s_last_fail_time = 0;
    static uint32_t s_last_read_time = 0;
    static bool s_last_pressed = false;
    static uint16_t s_last_x = 0, s_last_y = 0;
    static uint32_t s_last_release_ms = 0;
    // Minimum gap between two consecutive presses. The user can't tap
    // a button twice in <150 ms intentionally — anything faster is
    // either a finger-bounce or a release glitch. Tap-spam protection.
    const uint32_t MIN_REPRESS_GAP_MS = 150;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Touch chip never identified during init — register reads can still
    // succeed against a bus-alive but glitched controller and return
    // garbage that decodes as "1 or 2 touches" (phantom presses). Bail
    // out here so the UI doesn't react to noise. Also retry init every
    // 5 s in the background so the chip can self-heal if it just had
    // a transient boot-time glitch — without this, a single bad cold
    // boot would leave the user with a non-touch screen until power
    // cycle.
    if (!s_touch_initialized) {
        *pressed = false;
        *x = 0;
        *y = 0;
        // Re-init runs in the caller's context (clock_task). The full
        // ft6336u_init() can spend 600+ ms in vTaskDelay (3 retries x
        // 200 ms reset pulses). Doing that every 5 s on a stuck chip
        // visibly stalls the UI. Just request that the deferred-init
        // task try again — only call ft6336u_init directly here as a
        // lightweight (single-attempt) probe, with no inner retries.
        // Done via static flag so we still self-heal but at most once.
        static uint32_t s_last_reinit_ms = 0;
        if (now - s_last_reinit_ms >= 10000) {
            s_last_reinit_ms = now;
            // Cheap probe: just see if the chip ACKs its address now.
            // Skip the full reset-pulse / chip-id retry sequence.
            if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
                s_touch_initialized = true;
                ESP_LOGI(TAG, "Touch chip came back online (cheap probe)");
            }
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Backoff after a read failure. 100 ms is short enough that the
    // user doesn't notice but long enough that we don't hammer a
    // genuinely wedged chip.
    if (s_last_fail_time != 0 && (now - s_last_fail_time) < 100) {
        *pressed = false;
        return ESP_FAIL;
    }
    // Rate-limit physical I2C reads. Faster cache = smoother feel:
    //   - Idle: 15 ms cache (~67 Hz) — taps register near-instantly.
    //   - Pressed: 10 ms cache (100 Hz) — drag/release tracks finger
    //     without visible lag.
    uint32_t cache_ms = s_last_pressed ? 10 : 15;
    if ((now - s_last_read_time) < cache_ms) {
        *pressed = s_last_pressed;
        *x = s_last_x;
        *y = s_last_y;
        return ESP_OK;
    }

    /* Camera SCCB takes priority — back off touch reads during the
     * ~3 s burst so they don't shred the OV5647 register sequence.
     * Returns the last cached value so the UI stays alive (just frozen
     * for those few seconds) without an error path.
     *
     * Safety net: bound the skip to 5 s. If camera_init exits via some
     * unhandled early-return path that leaves the flag true, touch
     * recovers on its own instead of locking out the user forever.
     * The first camera SCCB call after this watchdog kicks in still
     * works because we don't clear the flag here — just override the
     * skip. */
    static uint32_t s_sccb_skip_start = 0;
    if (g_camera_sccb_in_progress) {
        if (s_sccb_skip_start == 0) s_sccb_skip_start = now ? now : 1;
        if ((now - s_sccb_skip_start) < 5000) {
            *pressed = s_last_pressed;
            *x = s_last_x;
            *y = s_last_y;
            return ESP_OK;
        }
        /* 5 s exceeded — flag is stuck, log once and stop skipping. */
        static uint32_t s_last_warn_ms = 0;
        if ((now - s_last_warn_ms) > 30000) {
            ESP_LOGW(TAG, "g_camera_sccb_in_progress stuck >5s — bypassing skip to keep touch alive");
            s_last_warn_ms = now;
        }
    } else {
        s_sccb_skip_start = 0;
    }
    s_last_read_time = now;

    /* Auto-recover if the I2C bus wedges. Without this, a single SDA
     * glitch (e.g. servo current spike during dispense) leaves touch
     * dead until the user power-cycles the board. We count consecutive
     * read failures; after 8 in a row (~160 ms with 20 ms cache), call
     * i2c_manager_recover_bus() to bit-bang SDA free + recreate the
     * master bus. 30 s cool-down between recovery attempts so a truly
     * dead bus doesn't loop forever. */
    static uint32_t s_consec_fails = 0;
    static uint32_t s_last_recover_ms = 0;

    uint8_t data[6];
    esp_err_t ret = i2c_manager_read_reg(ADDR_FT6336U, 0x02, data, 6);
    if (ret != ESP_OK) {
        *pressed = false;
        // 0 is the "no recent fail" sentinel, so guard against the
        // ~49.7 day tick wrap landing exactly on 0.
        s_last_fail_time = (now == 0) ? 1u : now;

        /* Distinguish bus-busy (other task holding I2C mutex) from
         * actual chip failure. ESP_ERR_TIMEOUT means the mutex couldn't
         * be acquired in 500 ms. The touch chip itself is fine, so
         * don't count it toward s_consec_fails (which would trigger
         * the heavy CTP_RST + bus-recovery sequence and leave touch
         * dead for ~10 s). The 100 ms backoff above naturally retries
         * once the bus is free. */
        if (ret == ESP_ERR_TIMEOUT) {
            return ret;
        }

        /* Don't count fails toward recovery while a servo ramp is in
         * progress. Servo PWM EMI couples onto the shared I2C bus and
         * causes transient NACKs / corrupted reads — those are
         * expected, not a chip failure. Without this guard, a single
         * 16-cycle return-pill (≈32 s of servo activity) was racking
         * up enough fails to trip the recovery and leave touch in
         * fallback mode after dispense. */
        if (pca9685_servo_busy_get()) {
            return ret;
        }

        s_consec_fails++;
        if (s_consec_fails >= 8 && (now - s_last_recover_ms) > 2000) {
            ESP_LOGW(TAG, "Touch read failed %lu times in a row — "
                          "attempting bus + CTP_RST recovery",
                     (unsigned long)s_consec_fails);
            /* Toggle CTP_RST GPIO to hard-reset the FT6336U chip itself,
             * then bit-bang SDA + recreate the I2C master bus. Bus-only
             * recovery wasn't enough — once the chip is wedged at the
             * register level it ignores I2C reads even on a clean bus.
             * Cool-down 2 s (was 30 s) so the user doesn't have to wait
             * half a minute with a frozen screen. Log spam is acceptable
             * compared to the freeze. */
            if (CTP_RST_PIN >= 0) {
                /* Longer pulse than before — runtime touch fail is
                 * usually deeper than init-time glitches and needs
                 * more time for the chip to fully reset. */
                gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            esp_err_t r = i2c_manager_recover_bus();
            ESP_LOGW(TAG, "Bus recovery -> %s", esp_err_to_name(r));
            s_last_recover_ms = now;
            s_consec_fails = 0;
            /* Force touch re-init on next call so the chip resyncs
             * after the bus recovery cycle. */
            s_touch_initialized = false;
            /* Re-arm the 3-second boot-mute so the freshly-reset chip
             * doesn't fire phantom presses while it stabilises. Without
             * this, the chip comes back in fallback mode and the
             * register noise during stabilisation gets interpreted as
             * a real tap (user reported "จอกดเอง" right after recovery). */
            s_boot_start_ms = 0;
            /* Immediate re-probe after recovery. Previously the cheap-probe
             * path (line ~172) was gated by a 10-second re-init throttle —
             * so a scheduled-slot Confirm popup arriving within 10 s of a
             * recovery would render with TOUCH STILL OFFLINE (the user saw
             * the Confirm page but taps did nothing). Probe right now so
             * the very next ft6336u_read_touch() call after we return is
             * already live. If the chip is still not responding we just
             * fall back to the existing 10-second cheap-probe retry. */
            if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
                s_touch_initialized = true;
                ESP_LOGI(TAG, "Touch chip responsive immediately after bus recovery");
            }
        }
        return ret;
    }
    /* Successful read — clear the failure streak. */
    s_consec_fails = 0;
    s_last_fail_time = 0;

    uint8_t touches = data[0] & 0x0F;
    // Note: weight register (data[5]) was tried as a phantom-touch filter
    // but in fallback-mode the chip returns weight==0 even for real
    // presses — using it disabled all touches. Rely on PRESS_DEBOUNCE
    // and the (0,0)-coord sanity check below instead.
    bool raw_press = (touches == 1 || touches == 2);
    uint16_t new_x = 0, new_y = 0;
    if (raw_press) {
        uint16_t raw_x = ((data[1] & 0x0F) << 8) | data[2];
        uint16_t raw_y = ((data[3] & 0x0F) << 8) | data[4];
        new_x = raw_y;
        new_y = 320 - raw_x;
        if (new_x > 480) new_x = 480;
        if (new_y > 320) new_y = 320;
        if (new_x == 0 && new_y == 0) raw_press = false;
    }
    /* Reject any "touches" count above 2 — FT6336U hardware max is 2,
     * so 3-15 means corrupted register read = phantom. */
    if (touches > 2) raw_press = false;

    /* Boot-time touch mute (1.5 s). Absorbs phantom presses while the
     * chip's own register settle finishes — keeps the user from having
     * to wait long after the splash before taps register. */
    if (s_boot_start_ms == 0) s_boot_start_ms = now;
    if ((now - s_boot_start_ms) < 1500) {
        raw_press = false;
    }

    /* PRESS_DEBOUNCE = 2 (~30 ms hold with cache_ms idle=15) + LOOSE
     * coord-stability check. The 8 s boot-mute above is the main
     * phantom-touch defense; debounce=2 keeps post-boot taps feeling
     * snappy. Real human taps last 150-200 ms minimum so 30 ms is
     * well within real-tap tolerance. */
    static int press_streak = 0;
    static int release_streak = 0;
    static uint16_t s_streak_x = 0, s_streak_y = 0;
    const int PRESS_DEBOUNCE = 2;
    const int RELEASE_DEBOUNCE = 1;
    const int COORD_STABILITY_PX = 150;
    if (raw_press) {
        if (press_streak == 0) {
            s_streak_x = new_x;
            s_streak_y = new_y;
            press_streak = 1;
        } else {
            int dx = (int)new_x - (int)s_streak_x;
            int dy = (int)new_y - (int)s_streak_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= COORD_STABILITY_PX && dy <= COORD_STABILITY_PX) {
                press_streak++;
                s_streak_x = new_x;
                s_streak_y = new_y;
            } else {
                press_streak = 1;
                s_streak_x = new_x;
                s_streak_y = new_y;
            }
        }
        release_streak = 0;
    } else {
        release_streak++;
        press_streak = 0;
    }

    if (s_last_pressed) {
        // Currently considered pressed: stay pressed unless we've seen
        // RELEASE_DEBOUNCE clean misses in a row (avoids brief read
        // glitches breaking a real long-press / drag).
        if (release_streak >= RELEASE_DEBOUNCE) {
            *pressed = false;
            s_last_release_ms = now;     // start tap-spam guard
        } else {
            *pressed = true;
            // Update coords if we have a fresh good sample.
            if (raw_press) {
                s_last_x = new_x;
                s_last_y = new_y;
            }
            *x = s_last_x;
            *y = s_last_y;
            s_last_read_time = now;
            return ESP_OK;
        }
    } else {
        // Currently idle: only register a press once the streak
        // reaches PRESS_DEBOUNCE AND we're past the tap-spam window.
        if (press_streak >= PRESS_DEBOUNCE) {
            if ((now - s_last_release_ms) < MIN_REPRESS_GAP_MS) {
                // Too soon after the previous release — treat as bounce
                // / accidental tap-spam and ignore.
                *pressed = false;
            } else {
                *pressed = true;
                s_last_x = new_x;
                s_last_y = new_y;
                *x = new_x;
                *y = new_y;
                s_last_pressed = true;
                return ESP_OK;
            }
        } else {
            *pressed = false;
        }
    }
    s_last_pressed = *pressed;
    if (!*pressed) {
        *x = 0;
        *y = 0;
    }
    return ESP_OK;
}
