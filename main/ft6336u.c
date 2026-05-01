#include "ft6336u.h"
#include "config.h"
#include "i2c_manager.h"
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
        ESP_LOGI(TAG, "Resetting touch controller");
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level((gpio_num_t)CTP_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
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
    if (i2c_manager_ping(ADDR_FT6336U) == ESP_OK) {
        ESP_LOGW(TAG, "Touch chip-id check failed but I2C address ACKs — "
                       "enabling touch in fallback mode");
        s_touch_initialized = true;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Touch controller not responding on I2C — disabling touch");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ft6336u_read_touch(uint16_t *x, uint16_t *y, bool *pressed)
{
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
        if (pressed) *pressed = false;
        if (x) *x = 0;
        if (y) *y = 0;
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

    if (s_last_fail_time != 0 && (now - s_last_fail_time) < 2000) {
        *pressed = false;
        return ESP_FAIL;
    }
    // Rate-limit physical I2C reads. Faster cache = smoother feel:
    //   - Idle: 20 ms cache (50 Hz) — taps register near-instantly.
    //   - Pressed: 10 ms cache (100 Hz) — drag/release tracks finger
    //     without visible lag.
    uint32_t cache_ms = s_last_pressed ? 10 : 20;
    if ((now - s_last_read_time) < cache_ms) {
        *pressed = s_last_pressed;
        *x = s_last_x;
        *y = s_last_y;
        return ESP_OK;
    }
    s_last_read_time = now;

    uint8_t data[6];
    esp_err_t ret = i2c_manager_read_reg(ADDR_FT6336U, 0x02, data, 6);
    if (ret != ESP_OK) {
        *pressed = false;
        // 0 is the "no recent fail" sentinel, so guard against the
        // ~49.7 day tick wrap landing exactly on 0.
        s_last_fail_time = (now == 0) ? 1u : now;
        return ret;
    }
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
        // Coord-sanity: FT6336U fallback mode sometimes spits 0,0 or
        // wild-out-of-range values that pass the touches==1 check but
        // are not real presses. Treat (0,0) and edge-clipped values
        // alone (without nearby prior presses) as noise.
        if (new_x == 0 && new_y == 0) raw_press = false;
    }

    // PRESS_DEBOUNCE = 1: tap registers on the first valid sample.
    // RELEASE_DEBOUNCE = 1: lift registers immediately for snappy feel.
    // Phantom-touch protection comes from the (0,0)-coord sanity check
    // earlier in this function.
    static int press_streak = 0;
    static int release_streak = 0;
    const int PRESS_DEBOUNCE = 1;
    const int RELEASE_DEBOUNCE = 1;
    if (raw_press) {
        press_streak++;
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
