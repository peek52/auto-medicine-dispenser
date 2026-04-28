#include "ft6336u.h"
#include "config.h"
#include "i2c_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FT6336U";

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
    ESP_LOGE(TAG, "Touch controller failed ID check after 3 retries — disabling touch");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ft6336u_read_touch(uint16_t *x, uint16_t *y, bool *pressed)
{
    static uint32_t s_last_fail_time = 0;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_last_fail_time != 0 && (now - s_last_fail_time) < 2000) {
        *pressed = false;
        return ESP_FAIL;
    }

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
    return ESP_OK;
}
