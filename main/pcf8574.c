#include "pcf8574.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "pcf8574";

esp_err_t pcf8574_set_all_input(void)
{
    uint8_t cmd = 0xFF;
    esp_err_t ret = i2c_manager_write(ADDR_PCF8574, &cmd, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_all_input failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcf8574_read(uint8_t *val_out)
{
    if (!val_out) return ESP_ERR_INVALID_ARG;

    /* PCF8574 read protocol:
     *  1. Write 0xFF to put all pins into quasi-bidirectional input mode.
     *  2. Pure I2C read (no register byte) — PCF8574 returns pin states.
     *
     * Do NOT use i2c_manager_read_reg() because it sends a register byte first.
     * PCF8574 has no registers; any byte sent to it is treated as output data.
     * Sending 0x00 would drive all outputs LOW and read back 0x00 forever.
     */

    // Step 1: set all pins to input high
    uint8_t cmd = 0xFF;
    esp_err_t ret = i2c_manager_write(ADDR_PCF8574, &cmd, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "write 0xFF failed: %s", esp_err_to_name(ret));
        *val_out = 0xFF;
        return ret;
    }

    // Step 2: pure read — returns actual pin state
    ret = i2c_manager_read(ADDR_PCF8574, val_out, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(ret));
        *val_out = 0xFF;
    }
    ESP_LOGD(TAG, "PCF8574 raw=0x%02X", *val_out);
    return ret;
}
