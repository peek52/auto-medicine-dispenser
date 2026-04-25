#include "vl53l0x_multi.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_manager.h"
#include "pill_sensor_status.h"
#include "tca9548a.h"
#include "netpie_mqtt.h"
#include "nvs.h"
#include "nvs_flash.h"

#define VL53_REG_SYSRANGE_START                         0x00
#define VL53_REG_SYSTEM_SEQUENCE_CONFIG                 0x01
#define VL53_REG_SYSTEM_INTERMEASUREMENT_PERIOD         0x04
#define VL53_REG_SYSTEM_INTERRUPT_CONFIG_GPIO           0x0A
#define VL53_REG_SYSTEM_INTERRUPT_CLEAR                 0x0B
#define VL53_REG_RESULT_INTERRUPT_STATUS                0x13
#define VL53_REG_RESULT_RANGE_STATUS                    0x14
#define VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH                0x84
#define VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV      0x89
#define VL53_REG_MSRC_CONFIG_CONTROL                    0x60
#define VL53_REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE      0x44
#define VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI   0x71
#define VL53_REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD        0x70
#define VL53_REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI     0x51
#define VL53_REG_PRE_RANGE_CONFIG_VCSEL_PERIOD          0x50
#define VL53_REG_MSRC_CONFIG_TIMEOUT_MACROP             0x46
#define VL53_REG_IDENTIFICATION_MODEL_ID                0xC0
#define VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0       0xB0
#define VL53_REG_GLOBAL_CONFIG_REF_EN_START_SELECT      0xB6
#define VL53_REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD    0x4E
#define VL53_REG_DYNAMIC_SPAD_REF_EN_START_OFFSET       0x4F
#define VL53_REG_OSC_CALIBRATE_VAL                      0xF8

#define VL53_MODEL_ID                                   0xEE
#define VL53_BOOT_RETRIES                               3
#define VL53_BOOT_RETRY_DELAY_MS                        50
#define VL53_RANGING_TIMEOUT_MS                         500
#define VL53_READ_INTERVAL_MS                           1000
#define VL53_MAX_VALID_MM                               2000
#define VL53_EMA_ALPHA                                  0.60f
#define VL53_I2C_SPEED_HZ                               I2C_FREQ_HZ
#define VL53_MISSING_RETRY_MS                           30000
#define VL53_MAX_MISSING_RETRIES                        5    // Stop retrying after N consecutive failures
#define VL53_CONTINUOUS_PERIOD_MS                       50
#define VL53_RESTART_AFTER_FAILS                        5
#define VL53_INVALID_GRACE_READS                        3
#define VL53_STABLE_TICKS_BEFORE_SYNC                   1

#define decodeVcselPeriod(reg_val)      (((reg_val) + 1U) << 1U)
#define calcMacroPeriod(vcsel_period_pclks) ((((uint32_t)2304U * (vcsel_period_pclks) * 1655U) + 500U) / 1000U)

static const char *TAG = "vl53_multi";

typedef struct {
    bool tcc;
    bool msrc;
    bool dss;
    bool pre_range;
    bool final_range;
} vl53_sequence_step_enables_t;

typedef struct {
    uint16_t pre_range_vcsel_period_pclks;
    uint16_t final_range_vcsel_period_pclks;
    uint16_t msrc_dss_tcc_mclks;
    uint16_t pre_range_mclks;
    uint16_t final_range_mclks;
    uint32_t msrc_dss_tcc_us;
    uint32_t pre_range_us;
    uint32_t final_range_us;
} vl53_sequence_step_timeouts_t;

// Every VL53L0X sits at I2C addr 0x29; the TCA9548A isolates them by channel.
// One shared device handle is enough because channel-select and each
// transaction share the same I2C mutex.
typedef struct {
    bool     present;
    bool     filter_ready;
    bool     permanently_missing;
    uint8_t  stop_variable;
    uint8_t  read_fail_count;
    uint8_t  invalid_sample_count;
    uint8_t  missing_retry_count;
    uint32_t measurement_timing_budget_us;
    float    filtered_mm;
} vl53_sensor_t;

static vl53_sensor_t s_sensors[PILL_SENSOR_COUNT];
static TickType_t s_retry_after_ticks[PILL_SENSOR_COUNT] = {0};
static i2c_master_dev_handle_t s_vl53_dev = NULL;

// ─── Low-level I/O ──────────────────────────────────────────────────────────

static esp_err_t vl53_ensure_dev_locked(void)
{
    if (s_vl53_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = VL53L0X_DEFAULT_ADDR,
        .scl_speed_hz    = VL53_I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &s_vl53_dev);
}

static void vl53_release_dev(void)
{
    if (!g_i2c_mutex) return;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    if (s_vl53_dev) {
        i2c_master_bus_rm_device(s_vl53_dev);
        s_vl53_dev = NULL;
    }
    xSemaphoreGive(g_i2c_mutex);
}

// Acquire mutex, select TCA channel, then run fn — all atomic under one lock.
// Prevents any other task from flipping the TCA channel mid-transaction.
static esp_err_t vl53_io(int ch, esp_err_t (*fn)(i2c_master_dev_handle_t dev, void *ctx), void *ctx)
{
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = tca9548a_select_channel_locked((uint8_t)ch);
    if (ret == ESP_OK) {
        ret = vl53_ensure_dev_locked();
        if (ret == ESP_OK) {
            ret = fn(s_vl53_dev, ctx);
        }
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

typedef struct {
    uint8_t reg;
    uint8_t *buf;
    size_t len;
} vl53_read_ctx_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
} vl53_write_ctx_t;

static esp_err_t vl53_write_impl(i2c_master_dev_handle_t dev, void *ctx)
{
    vl53_write_ctx_t *w = (vl53_write_ctx_t *)ctx;
    return i2c_master_transmit(dev, w->buf, w->len, 100);
}

static esp_err_t vl53_read_impl(i2c_master_dev_handle_t dev, void *ctx)
{
    vl53_read_ctx_t *r = (vl53_read_ctx_t *)ctx;
    esp_err_t ret = i2c_master_transmit(dev, &r->reg, 1, 100);
    if (ret != ESP_OK) return ret;
    // Wait for the transmit ISR to fully retire — this barrier is the
    // documented way to synchronize before issuing the next operation
    // and avoids the i2c_isr_receive_handler ptr=NULL race.
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (bus) i2c_master_bus_wait_all_done(bus, 100);
    return i2c_master_receive(dev, r->buf, r->len, 100);
}

static esp_err_t vl53_write_reg(int ch, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    vl53_write_ctx_t ctx = { .buf = buf, .len = sizeof(buf) };
    return vl53_io(ch, vl53_write_impl, &ctx);
}

static esp_err_t vl53_write_reg16(int ch, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)value };
    vl53_write_ctx_t ctx = { .buf = buf, .len = sizeof(buf) };
    return vl53_io(ch, vl53_write_impl, &ctx);
}

static esp_err_t vl53_write_reg32(int ch, uint8_t reg, uint32_t value)
{
    uint8_t buf[5] = {
        reg,
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)value,
    };
    vl53_write_ctx_t ctx = { .buf = buf, .len = sizeof(buf) };
    return vl53_io(ch, vl53_write_impl, &ctx);
}

static esp_err_t vl53_write_multi(int ch, uint8_t reg, const uint8_t *src, size_t count)
{
    if (count > 6) return ESP_ERR_INVALID_ARG;
    uint8_t buf[1 + 6];
    buf[0] = reg;
    memcpy(&buf[1], src, count);
    vl53_write_ctx_t ctx = { .buf = buf, .len = 1 + count };
    return vl53_io(ch, vl53_write_impl, &ctx);
}

static esp_err_t vl53_read_reg(int ch, uint8_t reg, uint8_t *value)
{
    vl53_read_ctx_t ctx = { .reg = reg, .buf = value, .len = 1 };
    return vl53_io(ch, vl53_read_impl, &ctx);
}

static esp_err_t vl53_read_reg16(int ch, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2] = {0};
    vl53_read_ctx_t ctx = { .reg = reg, .buf = buf, .len = sizeof(buf) };
    esp_err_t ret = vl53_io(ch, vl53_read_impl, &ctx);
    if (ret == ESP_OK) {
        *value = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    }
    return ret;
}

static esp_err_t vl53_read_multi(int ch, uint8_t reg, uint8_t *dst, size_t count)
{
    vl53_read_ctx_t ctx = { .reg = reg, .buf = dst, .len = count };
    return vl53_io(ch, vl53_read_impl, &ctx);
}

static bool vl53_wait_for_model_id(int ch)
{
    uint8_t model_id = 0;
    for (int attempt = 0; attempt < VL53_BOOT_RETRIES; ++attempt) {
        if (vl53_read_reg(ch, VL53_REG_IDENTIFICATION_MODEL_ID, &model_id) == ESP_OK &&
            model_id == VL53_MODEL_ID) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(VL53_BOOT_RETRY_DELAY_MS));
    }
    return false;
}

// ─── Timeout encode/decode helpers ──────────────────────────────────────────

static uint16_t vl53_decode_timeout(uint16_t reg_val)
{
    return (uint16_t)((reg_val & 0x00FFU) << (uint16_t)((reg_val & 0xFF00U) >> 8)) + 1U;
}

static uint16_t vl53_encode_timeout(uint32_t timeout_mclks)
{
    if (timeout_mclks == 0) return 0;
    uint32_t ls_byte = timeout_mclks - 1U;
    uint16_t ms_byte = 0;
    while ((ls_byte & 0xFFFFFF00U) > 0U) {
        ls_byte >>= 1;
        ms_byte++;
    }
    return (uint16_t)((ms_byte << 8) | (ls_byte & 0xFFU));
}

static uint32_t vl53_timeout_mclks_to_microseconds(uint16_t timeout_period_mclks, uint8_t vcsel_period_pclks)
{
    uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);
    return ((uint32_t)timeout_period_mclks * macro_period_ns + 500U) / 1000U;
}

static uint32_t vl53_timeout_microseconds_to_mclks(uint32_t timeout_period_us, uint8_t vcsel_period_pclks)
{
    uint32_t macro_period_ns = calcMacroPeriod(vcsel_period_pclks);
    return ((timeout_period_us * 1000U) + (macro_period_ns / 2U)) / macro_period_ns;
}

// ─── VL53L0X init / configuration (STMicro sequence) ────────────────────────

static bool vl53_set_signal_rate_limit(int ch, float limit_mcps)
{
    if (limit_mcps < 0.0f || limit_mcps > 511.99f) return false;
    uint16_t limit_fixed = (uint16_t)(limit_mcps * (1 << 7));
    return vl53_write_reg16(ch, VL53_REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE, limit_fixed) == ESP_OK;
}

static bool vl53_get_spad_info(int ch, uint8_t *count, bool *type_is_aperture)
{
    uint8_t tmp = 0;
    int waited_ms = 0;

    if (vl53_write_reg(ch, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x06) != ESP_OK) return false;

    if (vl53_read_reg(ch, 0x83, &tmp) != ESP_OK) return false;
    if (vl53_write_reg(ch, 0x83, tmp | 0x04) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x07) != ESP_OK ||
        vl53_write_reg(ch, 0x81, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0x94, 0x6B) != ESP_OK ||
        vl53_write_reg(ch, 0x83, 0x00) != ESP_OK) return false;

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(ch, 0x83, &tmp) == ESP_OK && tmp != 0x00) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    if (tmp == 0x00 ||
        vl53_write_reg(ch, 0x83, 0x01) != ESP_OK ||
        vl53_read_reg(ch, 0x92, &tmp) != ESP_OK) return false;

    *count = tmp & 0x7F;
    *type_is_aperture = ((tmp >> 7) & 0x01U) != 0;

    return vl53_write_reg(ch, 0x81, 0x00) == ESP_OK &&
           vl53_write_reg(ch, 0xFF, 0x06) == ESP_OK &&
           vl53_read_reg(ch, 0x83, &tmp) == ESP_OK &&
           vl53_write_reg(ch, 0x83, tmp & ~0x04U) == ESP_OK &&
           vl53_write_reg(ch, 0xFF, 0x01) == ESP_OK &&
           vl53_write_reg(ch, 0x00, 0x01) == ESP_OK &&
           vl53_write_reg(ch, 0xFF, 0x00) == ESP_OK &&
           vl53_write_reg(ch, 0x80, 0x00) == ESP_OK;
}

static bool vl53_get_sequence_step_enables(int ch, vl53_sequence_step_enables_t *enables)
{
    uint8_t cfg = 0;
    if (vl53_read_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, &cfg) != ESP_OK) return false;
    enables->tcc         = ((cfg >> 4) & 0x1U) != 0;
    enables->dss         = ((cfg >> 3) & 0x1U) != 0;
    enables->msrc        = ((cfg >> 2) & 0x1U) != 0;
    enables->pre_range   = ((cfg >> 6) & 0x1U) != 0;
    enables->final_range = ((cfg >> 7) & 0x1U) != 0;
    return true;
}

static bool vl53_get_sequence_step_timeouts(int ch,
                                            const vl53_sequence_step_enables_t *enables,
                                            vl53_sequence_step_timeouts_t *timeouts)
{
    uint8_t  reg8 = 0;
    uint16_t reg16 = 0;

    if (vl53_read_reg(ch, VL53_REG_PRE_RANGE_CONFIG_VCSEL_PERIOD, &reg8) != ESP_OK) return false;
    timeouts->pre_range_vcsel_period_pclks = decodeVcselPeriod(reg8);

    if (vl53_read_reg(ch, VL53_REG_MSRC_CONFIG_TIMEOUT_MACROP, &reg8) != ESP_OK) return false;
    timeouts->msrc_dss_tcc_mclks = reg8 + 1U;
    timeouts->msrc_dss_tcc_us = vl53_timeout_mclks_to_microseconds(
        timeouts->msrc_dss_tcc_mclks, (uint8_t)timeouts->pre_range_vcsel_period_pclks);

    if (vl53_read_reg16(ch, VL53_REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI, &reg16) != ESP_OK) return false;
    timeouts->pre_range_mclks = vl53_decode_timeout(reg16);
    timeouts->pre_range_us = vl53_timeout_mclks_to_microseconds(
        timeouts->pre_range_mclks, (uint8_t)timeouts->pre_range_vcsel_period_pclks);

    if (vl53_read_reg(ch, VL53_REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD, &reg8) != ESP_OK) return false;
    timeouts->final_range_vcsel_period_pclks = decodeVcselPeriod(reg8);

    if (vl53_read_reg16(ch, VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI, &reg16) != ESP_OK) return false;
    timeouts->final_range_mclks = vl53_decode_timeout(reg16);
    if (enables->pre_range) timeouts->final_range_mclks -= timeouts->pre_range_mclks;
    timeouts->final_range_us = vl53_timeout_mclks_to_microseconds(
        timeouts->final_range_mclks, (uint8_t)timeouts->final_range_vcsel_period_pclks);

    return true;
}

static bool vl53_get_measurement_timing_budget(int ch, vl53_sensor_t *sensor)
{
    static const uint16_t StartOverhead = 1910, EndOverhead = 960;
    static const uint16_t MsrcOverhead = 660, TccOverhead = 590, DssOverhead = 690;
    static const uint16_t PreRangeOverhead = 660, FinalRangeOverhead = 550;

    vl53_sequence_step_enables_t en;
    vl53_sequence_step_timeouts_t to;
    if (!vl53_get_sequence_step_enables(ch, &en) ||
        !vl53_get_sequence_step_timeouts(ch, &en, &to)) return false;

    uint32_t budget = StartOverhead + EndOverhead;
    if (en.tcc)         budget += to.msrc_dss_tcc_us + TccOverhead;
    if (en.dss)         budget += 2U * (to.msrc_dss_tcc_us + DssOverhead);
    else if (en.msrc)   budget += to.msrc_dss_tcc_us + MsrcOverhead;
    if (en.pre_range)   budget += to.pre_range_us + PreRangeOverhead;
    if (en.final_range) budget += to.final_range_us + FinalRangeOverhead;

    sensor->measurement_timing_budget_us = budget;
    return true;
}

static bool vl53_set_measurement_timing_budget(int ch, vl53_sensor_t *sensor, uint32_t budget_us)
{
    static const uint16_t StartOverhead = 1910, EndOverhead = 960;
    static const uint16_t MsrcOverhead = 660, TccOverhead = 590, DssOverhead = 690;
    static const uint16_t PreRangeOverhead = 660, FinalRangeOverhead = 550;

    vl53_sequence_step_enables_t en;
    vl53_sequence_step_timeouts_t to;
    if (!vl53_get_sequence_step_enables(ch, &en) ||
        !vl53_get_sequence_step_timeouts(ch, &en, &to)) return false;

    uint32_t used = StartOverhead + EndOverhead;
    if (en.tcc)       used += to.msrc_dss_tcc_us + TccOverhead;
    if (en.dss)       used += 2U * (to.msrc_dss_tcc_us + DssOverhead);
    else if (en.msrc) used += to.msrc_dss_tcc_us + MsrcOverhead;
    if (en.pre_range) used += to.pre_range_us + PreRangeOverhead;
    if (!en.final_range) return false;

    used += FinalRangeOverhead;
    if (used > budget_us) return false;

    uint32_t final_timeout_us    = budget_us - used;
    uint32_t final_timeout_mclks = vl53_timeout_microseconds_to_mclks(
        final_timeout_us, (uint8_t)to.final_range_vcsel_period_pclks);
    if (en.pre_range) final_timeout_mclks += to.pre_range_mclks;

    if (vl53_write_reg16(ch, VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
                         vl53_encode_timeout(final_timeout_mclks)) != ESP_OK) return false;

    sensor->measurement_timing_budget_us = budget_us;
    return true;
}

static bool vl53_perform_single_ref_calibration(int ch, uint8_t vhv_init_byte)
{
    uint8_t status = 0;
    int waited_ms = 0;

    if (vl53_write_reg(ch, VL53_REG_SYSRANGE_START, (uint8_t)(0x01 | vhv_init_byte)) != ESP_OK) return false;

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(ch, VL53_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07U) != 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }
    if ((status & 0x07U) == 0) return false;

    return vl53_write_reg(ch, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) == ESP_OK &&
           vl53_write_reg(ch, VL53_REG_SYSRANGE_START, 0x00) == ESP_OK;
}

static bool vl53_read_range_continuous_mm(int ch, uint16_t *range_mm)
{
    int waited_ms = 0;
    uint8_t status = 0;

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(ch, VL53_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07U) != 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }
    if ((status & 0x07U) == 0) return false;

    if (vl53_read_reg16(ch, (uint8_t)(VL53_REG_RESULT_RANGE_STATUS + 10U), range_mm) != ESP_OK) return false;
    return vl53_write_reg(ch, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) == ESP_OK;
}

static bool vl53_start_continuous(int ch, uint8_t stop_variable, uint32_t period_ms)
{
    if (vl53_write_reg(ch, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(ch, 0x91, stop_variable) != ESP_OK ||
        vl53_write_reg(ch, 0x00, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(ch, 0x80, 0x00) != ESP_OK) return false;

    if (period_ms != 0) {
        uint16_t osc_cal = 0;
        if (vl53_read_reg16(ch, VL53_REG_OSC_CALIBRATE_VAL, &osc_cal) != ESP_OK) return false;
        if (osc_cal != 0) period_ms *= osc_cal;
        if (vl53_write_reg32(ch, VL53_REG_SYSTEM_INTERMEASUREMENT_PERIOD, period_ms) != ESP_OK ||
            vl53_write_reg(ch, VL53_REG_SYSRANGE_START, 0x04) != ESP_OK) return false;
    } else {
        if (vl53_write_reg(ch, VL53_REG_SYSRANGE_START, 0x02) != ESP_OK) return false;
    }
    return true;
}

#define LOG_FAIL(fmt, ...) do { ESP_LOGW(TAG, "Ch%d init fail: " fmt, ch, ##__VA_ARGS__); return false; } while (0)

static bool vl53_init_device(int ch, vl53_sensor_t *sensor)
{
    // Model ID was already verified by vl53_wait_for_model_id in the caller.
    uint8_t tmp = 0;
    if (vl53_read_reg(ch, VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, &tmp) != ESP_OK) LOG_FAIL("read VHV_CFG");
    if (vl53_write_reg(ch, VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, tmp | 0x01U) != ESP_OK) LOG_FAIL("write VHV_CFG");
    if (vl53_write_reg(ch, 0x88, 0x00) != ESP_OK) LOG_FAIL("write 0x88");
    if (vl53_write_reg(ch, 0x80, 0x01) != ESP_OK) LOG_FAIL("write 0x80 A");
    if (vl53_write_reg(ch, 0xFF, 0x01) != ESP_OK) LOG_FAIL("write 0xFF A");
    if (vl53_write_reg(ch, 0x00, 0x00) != ESP_OK) LOG_FAIL("write 0x00 A");

    if (vl53_read_reg(ch, 0x91, &sensor->stop_variable) != ESP_OK) LOG_FAIL("read 0x91 stop_var");
    if (vl53_write_reg(ch, 0x00, 0x01) != ESP_OK) LOG_FAIL("write 0x00 B");
    if (vl53_write_reg(ch, 0xFF, 0x00) != ESP_OK) LOG_FAIL("write 0xFF B");
    if (vl53_write_reg(ch, 0x80, 0x00) != ESP_OK) LOG_FAIL("write 0x80 B");
    if (vl53_read_reg(ch, VL53_REG_MSRC_CONFIG_CONTROL, &tmp) != ESP_OK) LOG_FAIL("read MSRC_CFG");
    if (vl53_write_reg(ch, VL53_REG_MSRC_CONFIG_CONTROL, tmp | 0x12U) != ESP_OK) LOG_FAIL("write MSRC_CFG");
    if (!vl53_set_signal_rate_limit(ch, 0.25f)) LOG_FAIL("set_signal_rate_limit");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xFF) != ESP_OK) LOG_FAIL("write SEQ_CFG 0xFF");

    uint8_t spad_count = 0;
    bool spad_type_is_aperture = false;
    if (!vl53_get_spad_info(ch, &spad_count, &spad_type_is_aperture)) LOG_FAIL("SPAD info");

    uint8_t ref_spad_map[6] = {0};
    if (vl53_read_multi(ch, VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, sizeof(ref_spad_map)) != ESP_OK) LOG_FAIL("read SPAD map");

    if (vl53_write_reg(ch, 0xFF, 0x01) != ESP_OK) LOG_FAIL("write 0xFF C");
    if (vl53_write_reg(ch, VL53_REG_DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00) != ESP_OK) LOG_FAIL("write SPAD_OFFSET");
    if (vl53_write_reg(ch, VL53_REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C) != ESP_OK) LOG_FAIL("write SPAD_NUM");
    if (vl53_write_reg(ch, 0xFF, 0x00) != ESP_OK) LOG_FAIL("write 0xFF D");
    if (vl53_write_reg(ch, VL53_REG_GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4) != ESP_OK) LOG_FAIL("write REF_EN");

    uint8_t first_spad_to_enable = spad_type_is_aperture ? 12U : 0U;
    uint8_t spads_enabled = 0;
    for (uint8_t i = 0; i < 48; i++) {
        if (i < first_spad_to_enable || spads_enabled == spad_count) {
            ref_spad_map[i / 8] &= (uint8_t)~(1U << (i % 8));
        } else if (((ref_spad_map[i / 8] >> (i % 8)) & 0x1U) != 0) {
            spads_enabled++;
        }
    }
    if (vl53_write_multi(ch, VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, sizeof(ref_spad_map)) != ESP_OK) LOG_FAIL("write SPAD enables");

    // STMicro DefaultTuningSettings: opaque register sequence required by the datasheet.
    static const struct { uint8_t reg; uint8_t value; } tuning[] = {
        {0xFF, 0x01}, {0x00, 0x00}, {0xFF, 0x00}, {0x09, 0x00}, {0x10, 0x00}, {0x11, 0x00},
        {0x24, 0x01}, {0x25, 0xFF}, {0x75, 0x00}, {0xFF, 0x01}, {0x4E, 0x2C}, {0x48, 0x00},
        {0x30, 0x20}, {0xFF, 0x00}, {0x30, 0x09}, {0x54, 0x00}, {0x31, 0x04}, {0x32, 0x03},
        {0x40, 0x83}, {0x46, 0x25}, {0x60, 0x00}, {0x27, 0x00}, {0x50, 0x06}, {0x51, 0x00},
        {0x52, 0x96}, {0x56, 0x08}, {0x57, 0x30}, {0x61, 0x00}, {0x62, 0x00}, {0x64, 0x00},
        {0x65, 0x00}, {0x66, 0xA0}, {0xFF, 0x01}, {0x22, 0x32}, {0x47, 0x14}, {0x49, 0xFF},
        {0x4A, 0x00}, {0xFF, 0x00}, {0x7A, 0x0A}, {0x7B, 0x00}, {0x78, 0x21}, {0xFF, 0x01},
        {0x23, 0x34}, {0x42, 0x00}, {0x44, 0xFF}, {0x45, 0x26}, {0x46, 0x05}, {0x40, 0x40},
        {0x0E, 0x06}, {0x20, 0x1A}, {0x43, 0x40}, {0xFF, 0x00}, {0x34, 0x03}, {0x35, 0x44},
        {0xFF, 0x01}, {0x31, 0x04}, {0x4B, 0x09}, {0x4C, 0x05}, {0x4D, 0x04}, {0xFF, 0x00},
        {0x44, 0x00}, {0x45, 0x20}, {0x47, 0x08}, {0x48, 0x28}, {0x67, 0x00}, {0x70, 0x04},
        {0x71, 0x01}, {0x72, 0xFE}, {0x76, 0x00}, {0x77, 0x00}, {0xFF, 0x01}, {0x0D, 0x01},
        {0xFF, 0x00}, {0x80, 0x01}, {0x01, 0xF8}, {0xFF, 0x01}, {0x8E, 0x01}, {0x00, 0x01},
        {0xFF, 0x00}, {0x80, 0x00},
    };
    for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); ++i) {
        if (vl53_write_reg(ch, tuning[i].reg, tuning[i].value) != ESP_OK) {
            ESP_LOGW(TAG, "Ch%d: tuning write failed at 0x%02X", ch, tuning[i].reg);
            return false;
        }
    }

    if (vl53_write_reg(ch, VL53_REG_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04) != ESP_OK) LOG_FAIL("write INTR_CFG");
    if (vl53_read_reg(ch, VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH, &tmp) != ESP_OK) LOG_FAIL("read HV_MUX");
    if (vl53_write_reg(ch, VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH, tmp & (uint8_t)~0x10U) != ESP_OK) LOG_FAIL("write HV_MUX");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) != ESP_OK) LOG_FAIL("write INTR_CLR");

    if (!vl53_get_measurement_timing_budget(ch, sensor)) LOG_FAIL("get_timing_budget");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xE8) != ESP_OK) LOG_FAIL("write SEQ 0xE8 A");
    if (!vl53_set_measurement_timing_budget(ch, sensor, sensor->measurement_timing_budget_us)) LOG_FAIL("set_timing_budget");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x01) != ESP_OK) LOG_FAIL("write SEQ 0x01");
    if (!vl53_perform_single_ref_calibration(ch, 0x40)) LOG_FAIL("ref_cal 0x40");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x02) != ESP_OK) LOG_FAIL("write SEQ 0x02");
    if (!vl53_perform_single_ref_calibration(ch, 0x00)) LOG_FAIL("ref_cal 0x00");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xE8) != ESP_OK) LOG_FAIL("write SEQ 0xE8 B");
    return true;
}

// ─── Sensor lifecycle ───────────────────────────────────────────────────────

static int vl53_apply_filter(int idx, uint16_t raw_mm)
{
    if (!s_sensors[idx].filter_ready) {
        s_sensors[idx].filtered_mm = (float)raw_mm;
        s_sensors[idx].filter_ready = true;
    } else {
        s_sensors[idx].filtered_mm =
            (s_sensors[idx].filtered_mm * (1.0f - VL53_EMA_ALPHA)) +
            ((float)raw_mm * VL53_EMA_ALPHA);
    }
    return (int)(s_sensors[idx].filtered_mm + 0.5f);
}

static void vl53_mark_sensor_missing(int idx)
{
    s_sensors[idx].present = false;
    s_sensors[idx].filter_ready = false;
    s_sensors[idx].read_fail_count = 0;
    s_sensors[idx].invalid_sample_count = 0;
    s_sensors[idx].measurement_timing_budget_us = 0;
    s_sensors[idx].filtered_mm = 0.0f;
    pill_sensor_status_mark_present(idx, false);
    s_retry_after_ticks[idx] = xTaskGetTickCount() + pdMS_TO_TICKS(VL53_MISSING_RETRY_MS);
}

static void vl53_mark_sensor_present(int idx)
{
    s_sensors[idx].present = true;
    s_sensors[idx].read_fail_count = 0;
    s_sensors[idx].invalid_sample_count = 0;
    s_retry_after_ticks[idx] = 0;
    pill_sensor_status_mark_present(idx, true);
}

static bool vl53_init_on_channel(int idx)
{
    if (!vl53_wait_for_model_id(idx)) {
        ESP_LOGW(TAG, "Ch%d: VL53 not found at 0x%02X", idx, VL53L0X_DEFAULT_ADDR);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    if (!vl53_init_device(idx, &s_sensors[idx])) {
        ESP_LOGW(TAG, "Ch%d: init failed", idx);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vl53_mark_sensor_present(idx);

    if (!vl53_start_continuous(idx, s_sensors[idx].stop_variable, VL53_CONTINUOUS_PERIOD_MS)) {
        ESP_LOGW(TAG, "Ch%d: continuous ranging start failed", idx);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(60));

    uint16_t raw_mm = 0;
    if (!vl53_read_range_continuous_mm(idx, &raw_mm)) {
        ESP_LOGW(TAG, "Ch%d: no first sample", idx);
        pill_sensor_status_set_reading(idx, -1, -1, false);
        return true;
    }

    pill_sensor_status_set_reading(idx, (int)raw_mm, vl53_apply_filter(idx, raw_mm), true);
    ESP_LOGI(TAG, "Ch%d: ready, first=%u mm", idx, raw_mm);
    return true;
}

static void vl53_release_xshut(void)
{
    // Drive XSHUT lines HIGH to bring every VL53L0X out of reset. Even though
    // we use the TCA9548A to isolate the bus, XSHUT still needs to be high for
    // the sensor to power its I2C front-end. Without this, all 6 channels NACK.
    static const gpio_num_t xshut[PILL_SENSOR_COUNT] = {
        VL53L0X_XSHUT_M1, VL53L0X_XSHUT_M2, VL53L0X_XSHUT_M3,
        VL53L0X_XSHUT_M4, VL53L0X_XSHUT_M5, VL53L0X_XSHUT_M6,
    };

    uint64_t mask = 0;
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        if (xshut[i] >= 0) mask |= 1ULL << (uint32_t)xshut[i];
    }
    if (mask == 0) return;

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        if (xshut[i] >= 0) gpio_set_level(xshut[i], 1);
    }
}

static void vl53_init_all(void)
{
    pill_sensor_status_init_defaults();
    vl53_release_xshut();
    tca9548a_disable_all();
    vl53_release_dev();
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        (void)vl53_init_on_channel(i);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void vl53_poll_all(void)
{
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        if (!s_sensors[i].present) {
            if (s_sensors[i].permanently_missing) continue;
            TickType_t now = xTaskGetTickCount();
            if (s_retry_after_ticks[i] == 0 || now >= s_retry_after_ticks[i]) {
                s_sensors[i].missing_retry_count++;
                if (s_sensors[i].missing_retry_count > VL53_MAX_MISSING_RETRIES) {
                    s_sensors[i].permanently_missing = true;
                    ESP_LOGW(TAG, "Ch%d: permanently marked missing after %d retries",
                             i, VL53_MAX_MISSING_RETRIES);
                    continue;
                }
                ESP_LOGI(TAG, "Ch%d: retry probe (%d/%d)", i,
                         s_sensors[i].missing_retry_count, VL53_MAX_MISSING_RETRIES);
                if (vl53_init_on_channel(i)) {
                    s_sensors[i].missing_retry_count = 0;
                }
            }
            continue;
        }

        uint16_t raw_mm = 0;
        if (!vl53_read_range_continuous_mm(i, &raw_mm)) {
            s_sensors[i].read_fail_count++;
            if (!s_sensors[i].filter_ready || s_sensors[i].read_fail_count >= VL53_INVALID_GRACE_READS) {
                pill_sensor_status_set_reading(i, -1, -1, false);
            }
            if (s_sensors[i].read_fail_count >= VL53_RESTART_AFTER_FAILS) {
                ESP_LOGW(TAG, "Ch%d: dropped after %d fails", i, VL53_RESTART_AFTER_FAILS);
                vl53_mark_sensor_missing(i);
            }
            continue;
        }

        s_sensors[i].read_fail_count = 0;
        if (raw_mm == 0 || raw_mm > VL53_MAX_VALID_MM) {
            s_sensors[i].invalid_sample_count++;
            if (!s_sensors[i].filter_ready || s_sensors[i].invalid_sample_count >= VL53_INVALID_GRACE_READS) {
                pill_sensor_status_set_reading(i, (int)raw_mm, -1, false);
            }
            continue;
        }

        s_sensors[i].invalid_sample_count = 0;
        pill_sensor_status_set_reading(i, (int)raw_mm, vl53_apply_filter(i, raw_mm), true);
    }
}

static void vl53_task(void *arg)
{
    (void)arg;

    // Pill count needs to remain stable for N polls before we push to shadow.
    // With VL53_READ_INTERVAL_MS = 1000 and threshold 3, that's ~3 seconds of stability.
    static int s_stable_count[PILL_SENSOR_COUNT];
    static int s_stable_ticks[PILL_SENSOR_COUNT];
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        s_stable_count[i] = -1;
        s_stable_ticks[i] = 0;
    }

    ESP_LOGI(TAG, "Starting VL53L0X poll task");
    while (1) {
        vl53_poll_all();

        const netpie_shadow_t *shadow = netpie_get_shadow();
        if (shadow && shadow->loaded) {
            for (int i = 0; i < PILL_SENSOR_COUNT; i++) {
                const pill_sensor_status_t *s = pill_sensor_status_get(i);
                if (!s || !s->valid || s->pill_count < 0) {
                    s_stable_count[i] = -1;
                    s_stable_ticks[i] = 0;
                    continue;
                }

                if (s->pill_count != s_stable_count[i]) {
                    s_stable_count[i] = s->pill_count;
                    s_stable_ticks[i] = 1;
                } else {
                    s_stable_ticks[i]++;
                }

                // While the user is on the meds-setup detail screen (or editing
                // any med field), don't auto-sync sensor → shadow count, otherwise
                // the +/- buttons "snap back" to the sensor reading and the user
                // can never enter a value. Sync resumes after they save / leave.
                extern bool ui_meds_edit_in_progress(void);
                if (ui_meds_edit_in_progress()) continue;

                if (s_stable_ticks[i] >= VL53_STABLE_TICKS_BEFORE_SYNC &&
                    shadow->med[i].count != s_stable_count[i]) {
                    ESP_LOGI(TAG, "Sensor %d sync: %d -> %d pills",
                             i + 1, shadow->med[i].count, s_stable_count[i]);
                    netpie_shadow_update_count(i + 1, s_stable_count[i]);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VL53_READ_INTERVAL_MS));
    }
}

// ─── Public API ─────────────────────────────────────────────────────────────

void vl53l0x_multi_bootstrap(void)
{
    ESP_LOGI(TAG, "Bootstrapping VL53L0X sensors via TCA9548A");
    vl53_init_all();
}

void vl53l0x_multi_start(void)
{
    static bool started = false;
    if (started) return;

    started = true;
    if (xTaskCreate(vl53_task, "vl53_task", 8192, NULL, 5, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "Failed to create VL53 task");
    }
}

// Persist per-channel calibration so it survives reboots. Key format:
//   namespace "vl53_cal", key "ch0".."ch5", blob = 3 × int16_t = full_dist_mm,
//   pill_height_mm, max_pills. Old defaults are restored if no blob exists.
typedef struct {
    int16_t full_dist_mm;
    int16_t pill_height_mm;
    int16_t max_pills;
} vl53_cal_blob_t;

static const char *VL53_NVS_NS = "vl53_cal";

static void vl53_cal_save_nvs(int ch, int full_dist_mm, int pill_height_mm, int max_pills)
{
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[8];
    snprintf(key, sizeof(key), "ch%d", ch);
    vl53_cal_blob_t blob = {
        .full_dist_mm   = (int16_t)full_dist_mm,
        .pill_height_mm = (int16_t)pill_height_mm,
        .max_pills      = (int16_t)max_pills,
    };
    nvs_set_blob(h, key, &blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}

void vl53l0x_load_calibration_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int ch = 0; ch < PILL_SENSOR_COUNT; ++ch) {
        char key[8];
        snprintf(key, sizeof(key), "ch%d", ch);
        vl53_cal_blob_t blob;
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, key, &blob, &sz) == ESP_OK && sz == sizeof(blob) &&
            blob.full_dist_mm > 0 && blob.pill_height_mm > 0 && blob.max_pills > 0) {
            pill_sensor_status_set_config(ch, blob.full_dist_mm, blob.pill_height_mm, blob.max_pills);
            ESP_LOGI(TAG, "Ch%d cal loaded from NVS: full=%dmm height=%dmm max=%d",
                     ch, blob.full_dist_mm, blob.pill_height_mm, blob.max_pills);
        }
    }
    nvs_close(h);
}

void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills)
{
    pill_sensor_status_set_config(ch, full_dist_mm, pill_height_mm, max_pills);
    vl53_cal_save_nvs(ch, full_dist_mm, pill_height_mm, max_pills);
}
