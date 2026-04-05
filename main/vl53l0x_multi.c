#include "vl53l0x_multi.h"

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_manager.h"
#include "pill_sensor_status.h"

#define VL53L0X_REG_SYSRANGE_START            0x00
#define VL53L0X_REG_SYSTEM_INTERRUPT_CLEAR    0x0B
#define VL53L0X_REG_RESULT_INTERRUPT_STATUS   0x13
#define VL53L0X_REG_RESULT_RANGE_MM           0x1E
#define VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS  0x8A
#define VL53L0X_INIT_DELAY_MS                 60
#define VL53L0X_MEASURE_TIMEOUT_MS            80
#define VL53L0X_READ_INTERVAL_MS              500
#define VL53L0X_MAX_VALID_MM                  2000
#define VL53L0X_EMA_ALPHA                     0.20f
#define VL53L0X_BOOT_RETRIES                  5
#define VL53L0X_BOOT_RETRY_DELAY_MS           30
#define VL53L0X_MAX_READ_FAILS                2
#define VL53L0X_I2C_SPEED_HZ                  100000

static const char *TAG = "vl53_multi";

typedef struct {
    uint8_t xshut_pin;
    uint8_t address;
    bool present;
    bool filter_ready;
    uint8_t read_fail_count;
    float filtered_mm;
} vl53_sensor_t;

static vl53_sensor_t s_sensors[PILL_SENSOR_COUNT] = {
    { VL53L0X_XSHUT_M1, VL53L0X_ADDR_M1, false, false, 0, 0.0f },
    { VL53L0X_XSHUT_M2, VL53L0X_ADDR_M2, false, false, 0, 0.0f },
    { VL53L0X_XSHUT_M3, VL53L0X_ADDR_M3, false, false, 0, 0.0f },
    { VL53L0X_XSHUT_M4, VL53L0X_ADDR_M4, false, false, 0, 0.0f },
    { VL53L0X_XSHUT_M5, VL53L0X_ADDR_M5, false, false, 0, 0.0f },
    { VL53L0X_XSHUT_M6, VL53L0X_ADDR_M6, false, false, 0, 0.0f },
};

static esp_err_t vl53_with_device(uint8_t addr, esp_err_t (*fn)(i2c_master_dev_handle_t dev, void *ctx), void *ctx)
{
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = VL53L0X_I2C_SPEED_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        ret = fn(dev, ctx);
        i2c_master_bus_rm_device(dev);
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

static esp_err_t vl53_write_impl(i2c_master_dev_handle_t dev, void *ctx)
{
    const uint8_t *buf = (const uint8_t *)ctx;
    return i2c_master_transmit(dev, buf, 2, 100);
}

typedef struct {
    uint8_t reg;
    uint8_t *buf;
    size_t len;
} vl53_read_ctx_t;

static esp_err_t vl53_read_impl(i2c_master_dev_handle_t dev, void *ctx)
{
    vl53_read_ctx_t *read_ctx = (vl53_read_ctx_t *)ctx;
    return i2c_master_transmit_receive(dev, &read_ctx->reg, 1, read_ctx->buf, read_ctx->len, 100);
}

static esp_err_t vl53_probe(uint8_t addr)
{
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_probe(bus, addr, 100);
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

static esp_err_t vl53_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return vl53_with_device(addr, vl53_write_impl, buf);
}

static esp_err_t vl53_read_reg(uint8_t addr, uint8_t reg, uint8_t *value)
{
    vl53_read_ctx_t ctx = {
        .reg = reg,
        .buf = value,
        .len = 1,
    };
    return vl53_with_device(addr, vl53_read_impl, &ctx);
}

static esp_err_t vl53_read_reg16(uint8_t addr, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2] = {0};
    vl53_read_ctx_t ctx = {
        .reg = reg,
        .buf = buf,
        .len = sizeof(buf),
    };
    esp_err_t ret = vl53_with_device(addr, vl53_read_impl, &ctx);
    if (ret == ESP_OK) {
        *value = (uint16_t)((buf[0] << 8) | buf[1]);
    }
    return ret;
}

static bool vl53_wait_measurement_ready(uint8_t addr)
{
    uint8_t status = 0;
    int waited_ms = 0;

    while (waited_ms < VL53L0X_MEASURE_TIMEOUT_MS) {
        if (vl53_read_reg(addr, VL53L0X_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07)) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    return false;
}

static bool vl53_read_single_mm(uint8_t addr, uint16_t *range_mm)
{
    if (vl53_write_reg(addr, VL53L0X_REG_SYSRANGE_START, 0x01) != ESP_OK) {
        return false;
    }

    if (!vl53_wait_measurement_ready(addr)) {
        return false;
    }

    if (vl53_read_reg16(addr, VL53L0X_REG_RESULT_RANGE_MM, range_mm) != ESP_OK) {
        return false;
    }

    (void)vl53_write_reg(addr, VL53L0X_REG_SYSTEM_INTERRUPT_CLEAR, 0x01);
    return true;
}

static int vl53_apply_filter(int idx, uint16_t raw_mm)
{
    if (!s_sensors[idx].filter_ready) {
        s_sensors[idx].filtered_mm = (float)raw_mm;
        s_sensors[idx].filter_ready = true;
    } else {
        s_sensors[idx].filtered_mm =
            (s_sensors[idx].filtered_mm * (1.0f - VL53L0X_EMA_ALPHA)) +
            ((float)raw_mm * VL53L0X_EMA_ALPHA);
    }

    return (int)(s_sensors[idx].filtered_mm + 0.5f);
}

static void vl53_mark_sensor_missing(int idx)
{
    s_sensors[idx].present = false;
    s_sensors[idx].filter_ready = false;
    s_sensors[idx].read_fail_count = 0;
    s_sensors[idx].filtered_mm = 0.0f;
    gpio_set_level((gpio_num_t)s_sensors[idx].xshut_pin, 0);
    pill_sensor_status_mark_present(idx, false);
}

static void vl53_mark_sensor_present(int idx)
{
    s_sensors[idx].present = true;
    s_sensors[idx].read_fail_count = 0;
    pill_sensor_status_mark_present(idx, true);
}

static bool vl53_assign_address(int idx)
{
    gpio_set_level((gpio_num_t)s_sensors[idx].xshut_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(VL53L0X_INIT_DELAY_MS));

    esp_err_t ping_ret = ESP_FAIL;
    for (int attempt = 0; attempt < VL53L0X_BOOT_RETRIES; ++attempt) {
        ping_ret = vl53_probe(VL53L0X_DEFAULT_ADDR);
        if (ping_ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(VL53L0X_BOOT_RETRY_DELAY_MS));
    }

    if (ping_ret != ESP_OK) {
        ESP_LOGW(TAG, "Module %d did not answer at default addr 0x%02X", idx + 1, VL53L0X_DEFAULT_ADDR);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    if (vl53_write_reg(VL53L0X_DEFAULT_ADDR, VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS, s_sensors[idx].address) != ESP_OK) {
        ESP_LOGW(TAG, "Module %d address change to 0x%02X failed", idx + 1, s_sensors[idx].address);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    if (vl53_probe(s_sensors[idx].address) != ESP_OK) {
        ESP_LOGW(TAG, "Module %d did not answer at new addr 0x%02X", idx + 1, s_sensors[idx].address);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vl53_mark_sensor_present(idx);
    uint16_t raw_mm = 0;
    if (!vl53_read_single_mm(s_sensors[idx].address, &raw_mm)) {
        ESP_LOGW(TAG, "Module %d answered at 0x%02X but failed first measurement", idx + 1, s_sensors[idx].address);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    pill_sensor_status_set_reading(idx, (int)raw_mm, vl53_apply_filter(idx, raw_mm), true);
    ESP_LOGI(TAG, "Module %d ready at 0x%02X", idx + 1, s_sensors[idx].address);
    return true;
}

static void vl53_init_gpio(void)
{
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_sensors[i].xshut_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level((gpio_num_t)s_sensors[i].xshut_pin, 0);
    }
}

void vl53l0x_multi_prepare_pins(void)
{
    vl53_init_gpio();
}

static void vl53_init_all(void)
{
    pill_sensor_status_init_defaults();
    vl53l0x_multi_prepare_pins();
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        (void)vl53_assign_address(i);
    }
}

static void vl53_poll_all(void)
{
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        if (!s_sensors[i].present) {
            continue;
        }

        uint16_t raw_mm = 0;
        if (!vl53_read_single_mm(s_sensors[i].address, &raw_mm)) {
            s_sensors[i].read_fail_count++;
            ESP_LOGW(TAG, "Module %d read timeout/fail (%u)", i + 1, s_sensors[i].read_fail_count);
            pill_sensor_status_set_reading(i, -1, -1, false);
            if (s_sensors[i].read_fail_count >= VL53L0X_MAX_READ_FAILS) {
                ESP_LOGW(TAG, "Module %d disabled after repeated read failures", i + 1);
                vl53_mark_sensor_missing(i);
            }
            continue;
        }

        s_sensors[i].read_fail_count = 0;

        if (raw_mm == 0 || raw_mm > VL53L0X_MAX_VALID_MM) {
            pill_sensor_status_set_reading(i, (int)raw_mm, -1, false);
            continue;
        }

        pill_sensor_status_set_reading(i, (int)raw_mm, vl53_apply_filter(i, raw_mm), true);
    }
}

static void vl53_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Starting VL53L0X poll task");

    while (1) {
        vl53_poll_all();
        vTaskDelay(pdMS_TO_TICKS(VL53L0X_READ_INTERVAL_MS));
    }
}

void vl53l0x_multi_bootstrap(void)
{
    ESP_LOGI(TAG, "Bootstrapping VL53L0X sensors");
    vl53_init_all();
}

void vl53l0x_multi_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }

    started = true;
    if (xTaskCreate(vl53_task, "vl53_task", 4096, NULL, 5, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "Failed to create VL53 task");
    }
}
