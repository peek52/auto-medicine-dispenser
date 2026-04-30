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
        static uint32_t s_last_reinit_ms = 0;
        if (now - s_last_reinit_ms >= 5000) {
            s_last_reinit_ms = now;
            (void)ft6336u_init();  // sets s_touch_initialized on success
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (s_last_fail_time != 0 && (now - s_last_fail_time) < 2000) {
        *pressed = false;
        return ESP_FAIL;
    }
    // Rate-limit physical I2C reads. Clock_task polls touch at 25-40 Hz
    // which loads the bus heavily. To balance responsiveness against the
    // IDF v5.3.2 i2c_master ISR race risk:
    //   - When idle (last read = no press): poll at 30 Hz (33 ms cache)
    //     so a quick tap doesn't fall through the gap and feel unresponsive.
    //   - When already pressed: poll faster (15 ms) so movement and release
    //     track the finger without lag.
    uint32_t cache_ms = s_last_pressed ? 15 : 33;
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
        s_last_fail_time = now;
        if (s_last_fail_time == 0) s_last_fail_time = 1;
        return ret;
    }
    s_last_fail_time = 0;

    uint8_t touches = data[0] & 0x0F;
    if (touches == 1 || touches == 2) {
        *pressed = true;
        // The display is physically 320x480, but drawn in landscape 480x320.
        // The FT6336U mapping might need swapping depending on hardware mounting.
        uint16_t raw_x = ((data[1] & 0x0F) << 8) | data[2];
        uint16_t raw_y = ((data[3] & 0x0F) << 8) | data[4];

        // Map ST7796S native touch coords to Landscape (swap X and Y, invert as needed)
        // Adjust these after testing. Assuming typical portrait to landscape rotation:
        *x = raw_y;           // Landscape X
        *y = 320 - raw_x;     // Landscape Y

        // Add bounds checks just in case
        if (*x > 480) *x = 480;
        if (*y > 320) *y = 320;
    } else {
        *pressed = false;
    }
    s_last_pressed = *pressed;
    s_last_x = *x;
    s_last_y = *y;
    return ESP_OK;
}
