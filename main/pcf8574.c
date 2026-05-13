#include "pcf8574.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pcf8574";

// Once the I2C bus latches into ESP_ERR_INVALID_STATE, every subsequent
// PCF8574 call returns instantly and floods the log + bus. The dispenser
// IR-sense loop calls pcf8574_read() at >100 Hz when it's hunting for a
// pill, so a wedged bus turns into a write-storm that starves every other
// I2C client (touch goes phantom, RTC clock garbles). Mute repeated
// failures: log only the first INVALID_STATE per second, return cached
// 0xFF (= "no pill detected") fast, no i2c_manager call at all.
static volatile bool s_in_error_mode = false;
static volatile int64_t s_error_since_us = 0;
static volatile int64_t s_last_warn_us = 0;
// Reduced 1000→50 ms after dispense logs showed pills passing the IR
// beam during a 1 s backoff window were missed (Drop 3: ok=114
// fail=442 → bit5_low=0 even though a pill physically dropped).
// 50 ms keeps backoff useful (avoids flooding bus on a truly dead
// chip) while keeping poll-density high enough that pills almost
// always land in an "online" window.
#define PCF_ERROR_BACKOFF_MS 50

static portMUX_TYPE s_pcf_mux = portMUX_INITIALIZER_UNLOCKED;

static bool pcf_should_skip_call(void)
{
    if (!s_in_error_mode) return false;
    int64_t now = esp_timer_get_time();
    bool allow = false;
    taskENTER_CRITICAL(&s_pcf_mux);
    if ((now - s_error_since_us) > (PCF_ERROR_BACKOFF_MS * 1000LL)) {
        // Allow one probe per backoff window to detect bus recovery.
        s_error_since_us = now;
        allow = true;
    }
    taskEXIT_CRITICAL(&s_pcf_mux);
    return !allow;
}

static void pcf_note_result(esp_err_t ret, const char *what)
{
    int64_t now = esp_timer_get_time();
    if (ret == ESP_OK) {
        if (s_in_error_mode) {
            ESP_LOGI(TAG, "PCF8574 came back online");
        }
        s_in_error_mode = false;
        return;
    }
    if (!s_in_error_mode) {
        s_in_error_mode = true;
        s_error_since_us = now;
        ESP_LOGW(TAG, "%s failed: %s — backing off (will probe every %d ms)",
                 what, esp_err_to_name(ret), PCF_ERROR_BACKOFF_MS);
        s_last_warn_us = now;
    } else if ((now - s_last_warn_us) > (5LL * 1000 * 1000)) {
        ESP_LOGW(TAG, "%s still failing (%s) after %lld ms",
                 what, esp_err_to_name(ret),
                 (long long)((now - s_error_since_us) / 1000));
        s_last_warn_us = now;
    }
}

esp_err_t pcf8574_set_all_input(void)
{
    if (pcf_should_skip_call()) return ESP_ERR_INVALID_STATE;
    uint8_t cmd = 0xFF;
    esp_err_t ret = i2c_manager_write(ADDR_PCF8574, &cmd, 1);
    pcf_note_result(ret, "set_all_input");
    return ret;
}

// Set true while a servo ramp is in flight. During the ramp the
// servo PWM update coupling EMI into the PCF8574 supply makes the
// IR module's OUT line wobble (visible as the indicator LED blinking
// even when the IR beam is unbroken). Reads return 0xFF (no-pill)
// during this window so the dispense loop's debounce counter doesn't
// latch onto noise spikes. Real pill drops are detected during the
// stable dwell phase after ramp completes.
static volatile bool s_block_during_servo_ramp = false;

void pcf8574_block_during_servo_ramp(bool blocked)
{
    s_block_during_servo_ramp = blocked;
}

bool pcf8574_is_servo_ramping(void)
{
    return s_block_during_servo_ramp;
}

esp_err_t pcf8574_read(uint8_t *val_out)
{
    if (!val_out) return ESP_ERR_INVALID_ARG;
    /* Previously: while s_block_during_servo_ramp was true (whole servo
     * ramp ~1.8 s), pcf8574_read forced 0xFF "no pill" to suppress EMI
     * spikes from PWM edges. BUG: the pill actually drops mid-ramp
     * (cup rotates ~45° → pill slides out → falls through the IR beam),
     * so the firmware was forcing 0xFF during the exact window when the
     * IR LED was lighting up on a real pill. User saw IR LED flash but
     * scheduler logged "❌ MISSED!".
     *
     * Real EMI spikes from servo PWM are <1 ms transients. The dispense
     * loop already requires 3 consecutive LOW samples (~30 ms at 100 Hz
     * polling) before declaring a detection, which rejects spikes but
     * catches real 50-100 ms pill drops. We don't need the blanket block
     * on top of debounce — keep the flag for callers that explicitly
     * want it via pcf8574_is_servo_ramping() (e.g., touch chip's
     * fallback path), but don't force the IR value here. */
    if (pcf_should_skip_call()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* PCF8574 quasi-bidirectional pins stay in input-high mode after the
     * boot-time pcf8574_set_all_input() write — the firmware never drives
     * them as outputs, so we don't need to re-send 0xFF before every read.
     * Cuts I2C traffic in half during the dispense IR poll (was 200/sec
     * → now 100/sec) and removes the mutex-release window between
     * write+read where touch/RTC ops could slip in and add jitter to the
     * IR sample timing.
     *
     * Pure read only. Do NOT use i2c_manager_read_reg() — it sends a
     * register byte first, and PCF8574 treats any byte sent to it as
     * output data (would drive pins LOW and read back 0x00). */
    esp_err_t ret = i2c_manager_read(ADDR_PCF8574, val_out, 1);
    if (ret != ESP_OK) {
        pcf_note_result(ret, "read");
        /* Don't overwrite *val_out here. Caller checks the return code
         * and is expected to leave its IR state untouched on error
         * (no false consec_low reset, no false 0xFF "no pill" sample).
         * If caller ignores the error, the previous value remains. */
        return ret;
    }
    pcf_note_result(ESP_OK, "read");
    ESP_LOGD(TAG, "PCF8574 raw=0x%02X", *val_out);
    return ret;
}
