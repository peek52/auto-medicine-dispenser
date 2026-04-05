#include "i2c_manager.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "i2c_mgr";
static i2c_master_bus_handle_t s_bus_handle = NULL;

SemaphoreHandle_t g_i2c_mutex = NULL;

esp_err_t i2c_manager_init(void)
{
    g_i2c_mutex = xSemaphoreCreateMutex();
    if (!g_i2c_mutex) {
        ESP_LOGE(TAG, "Failed to create I2C mutex");
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port        = I2C_NUM_0,
        .sda_io_num      = I2C_SDA_PIN,
        .scl_io_num      = I2C_SCL_PIN,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    // Explicitly configure pins to avoid stuck bus and ensure pull-ups
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_SDA_PIN) | (1ULL << I2C_SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Clear I2C bus (9 clocks)
    gpio_set_level(I2C_SDA_PIN, 1);
    gpio_set_level(I2C_SCL_PIN, 1);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(I2C_SCL_PIN, 0); vTaskDelay(1);
        gpio_set_level(I2C_SCL_PIN, 1); vTaskDelay(1);
    }
    gpio_reset_pin(I2C_SDA_PIN);
    gpio_reset_pin(I2C_SCL_PIN);

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C_NUM_0 ready — SDA=%d SCL=%d @ %d Hz",
             I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_manager_get_bus_handle(void)
{
    return s_bus_handle;
}

esp_err_t i2c_manager_ping(uint8_t addr)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        ret = i2c_master_probe(s_bus_handle, addr, 20);
        i2c_master_bus_rm_device(dev);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit(dev, data, len, 50);
        i2c_master_bus_rm_device(dev);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit_receive(dev, &reg, 1, buf, len, 50);
        i2c_master_bus_rm_device(dev);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_read(uint8_t addr, uint8_t *buf, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev, buf, len, 50);
        i2c_master_bus_rm_device(dev);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

