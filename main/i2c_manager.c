#include "i2c_manager.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

static const char *TAG = "i2c_mgr";
static i2c_master_bus_handle_t s_bus_handle = NULL;

SemaphoreHandle_t g_i2c_mutex = NULL;

// Pulse SCL via direct GPIO to free a slave that is holding SDA low.
// Returns true once SDA goes high (bus released), false if still stuck.
static bool i2c_unstick_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_SDA_PIN) | (1ULL << I2C_SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(I2C_SDA_PIN, 1);
    gpio_set_level(I2C_SCL_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    bool released = false;
    for (int round = 0; round < 4 && !released; round++) {
        if (gpio_get_level(I2C_SDA_PIN) == 1) { released = true; break; }
        for (int i = 0; i < 32; i++) {
            gpio_set_level(I2C_SCL_PIN, 0); esp_rom_delay_us(50);
            gpio_set_level(I2C_SCL_PIN, 1); esp_rom_delay_us(50);
            if (gpio_get_level(I2C_SDA_PIN) == 1) { released = true; break; }
        }
        // STOP: SDA low -> SCL high -> SDA high while SCL is high.
        gpio_set_level(I2C_SDA_PIN, 0); esp_rom_delay_us(50);
        gpio_set_level(I2C_SCL_PIN, 1); esp_rom_delay_us(50);
        gpio_set_level(I2C_SDA_PIN, 1); esp_rom_delay_us(50);
        if (gpio_get_level(I2C_SDA_PIN) == 1) { released = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!released) {
        ESP_LOGE(TAG, "Bus still stuck after 4 recovery rounds (SDA=%d SCL=%d)",
                 gpio_get_level(I2C_SDA_PIN), gpio_get_level(I2C_SCL_PIN));
    }
    gpio_reset_pin(I2C_SDA_PIN);
    gpio_reset_pin(I2C_SCL_PIN);
    return released;
}

static esp_err_t i2c_create_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port        = I2C_NUM_0,
        .sda_io_num      = I2C_SDA_PIN,
        .scl_io_num      = I2C_SCL_PIN,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus_handle);
}

esp_err_t i2c_manager_init(void)
{
    g_i2c_mutex = xSemaphoreCreateMutex();
    if (!g_i2c_mutex) {
        ESP_LOGE(TAG, "Failed to create I2C mutex");
        return ESP_ERR_NO_MEM;
    }

    i2c_unstick_gpio();

    esp_err_t ret = i2c_create_bus();
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

#define MAX_I2C_DEVICES 8
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev_handle;
} s_device_cache[MAX_I2C_DEVICES];
static int s_device_count = 0;

static i2c_master_dev_handle_t get_or_add_device(uint8_t addr)
{
    if (!s_bus_handle) return NULL;
    for (int i = 0; i < s_device_count; i++) {
        if (s_device_cache[i].addr == addr) {
            return s_device_cache[i].dev_handle;
        }
    }
    if (s_device_count >= MAX_I2C_DEVICES) return NULL;

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev) == ESP_OK) {
        s_device_cache[s_device_count].addr = addr;
        s_device_cache[s_device_count].dev_handle = dev;
        s_device_count++;
        return dev;
    }
    return NULL;
}

esp_err_t i2c_manager_ping(uint8_t addr)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    // Probe must not create/cache device handles. The boot-time scan checks many
    // empty addresses, and caching those dummy handles can fill the small device
    // cache before real devices like FT6336U/PCA9685/PCF8574/DS3231 are reached.
    esp_err_t ret = i2c_master_probe(s_bus_handle, addr, 100);
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    i2c_master_dev_handle_t dev = get_or_add_device(addr);
    if (dev) {
        ret = i2c_master_transmit(dev, data, len, 50);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_write_locked(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t dev = get_or_add_device(addr);
    if (!dev) return ESP_FAIL;
    return i2c_master_transmit(dev, data, len, 50);
}

esp_err_t i2c_manager_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    i2c_master_dev_handle_t dev = get_or_add_device(addr);
    if (dev) {
        ret = i2c_master_transmit(dev, &reg, 1, 50);
        if (ret == ESP_OK) {
            ret = i2c_master_receive(dev, buf, len, 50);
        }
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_read(uint8_t addr, uint8_t *buf, size_t len)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    i2c_master_dev_handle_t dev = get_or_add_device(addr);
    if (dev) {
        ret = i2c_master_receive(dev, buf, len, 50);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_manager_recover_bus(void)
{
    if (!g_i2c_mutex) return ESP_ERR_INVALID_STATE;

    // Use a timeout: if a task is hung holding the mutex (the very symptom
    // we're trying to recover from), don't block the watchdog forever.
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "recover_bus: cannot take mutex (deadlocked) — restarting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGW(TAG, "I2C runtime recovery: tearing down bus + unsticking SDA");

    for (int i = 0; i < s_device_count; i++) {
        if (s_device_cache[i].dev_handle) {
            i2c_master_bus_rm_device(s_device_cache[i].dev_handle);
            s_device_cache[i].dev_handle = NULL;
        }
    }
    s_device_count = 0;

    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }

    bool released = i2c_unstick_gpio();

    esp_err_t ret = i2c_create_bus();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover_bus: re-init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "recover_bus: bus re-initialized (sda_released=%d)", (int)released);
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}
