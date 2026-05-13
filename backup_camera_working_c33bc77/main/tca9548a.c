// ─────────────────────────────────────────────────────────────────────────────
//  tca9548a.c — TCA9548A I2C Multiplexer driver
//  Channel selection: write (1 << ch) to address ADDR_TCA9548A
//  Disable all: write 0x00
// ─────────────────────────────────────────────────────────────────────────────

#include "tca9548a.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tca9548a";

esp_err_t tca9548a_init(void)
{
    // 6 retries × 250 ms = 1.5 s window — enough for modules with slow
    // 3.3 V rail bring-up. Higher retry counts trigger an IDF v5.3.2
    // i2c_master ISR race when the bus is genuinely dead, but with the
    // bus-disable mitigation (i2c_manager_disable_bus) firing later on
    // total bus death we can afford a wider window for the live case.
    bool present = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (tca9548a_is_present()) { present = true; break; }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!present) {
        ESP_LOGW(TAG, "TCA9548A not found at 0x%02X after retries — VL53 sensors disabled",
                 ADDR_TCA9548A);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = tca9548a_disable_all();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TCA9548A init OK at 0x%02X", ADDR_TCA9548A);
    }
    return ret;
}

esp_err_t tca9548a_select_channel(uint8_t ch)
{
    if (ch > 7) return ESP_ERR_INVALID_ARG;
    uint8_t val = (uint8_t)(1u << ch);
    return i2c_manager_write(ADDR_TCA9548A, &val, 1);
}

esp_err_t tca9548a_select_channel_locked(uint8_t ch)
{
    if (ch > 7) return ESP_ERR_INVALID_ARG;
    uint8_t val = (uint8_t)(1u << ch);
    return i2c_manager_write_locked(ADDR_TCA9548A, &val, 1);
}

esp_err_t tca9548a_disable_all(void)
{
    uint8_t val = 0x00;
    return i2c_manager_write(ADDR_TCA9548A, &val, 1);
}

bool tca9548a_is_present(void)
{
    return (i2c_manager_ping(ADDR_TCA9548A) == ESP_OK);
}
