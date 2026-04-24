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

#define VL53_REG_SYSRANGE_START                         0x00
#define VL53_REG_SYSTEM_SEQUENCE_CONFIG                 0x01
#define VL53_REG_SYSTEM_INTERMEASUREMENT_PERIOD         0x04
#define VL53_REG_SYSTEM_INTERRUPT_CONFIG_GPIO           0x0A
#define VL53_REG_SYSTEM_INTERRUPT_CLEAR                 0x0B
#define VL53_REG_RESULT_INTERRUPT_STATUS                0x13
#define VL53_REG_RESULT_RANGE_STATUS                    0x14
#define VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH                0x84
#define VL53_REG_I2C_SLAVE_DEVICE_ADDRESS               0x8A
#define VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV      0x89
#define VL53_REG_MSRC_CONFIG_CONTROL                    0x60
#define VL53_REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE      0x44
#define VL53_REG_FINAL_RANGE_CONFIG_VALID_PHASE_LOW     0x47
#define VL53_REG_FINAL_RANGE_CONFIG_VALID_PHASE_HIGH    0x48
#define VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI   0x71
#define VL53_REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD        0x70
#define VL53_REG_PRE_RANGE_CONFIG_VALID_PHASE_LOW       0x56
#define VL53_REG_PRE_RANGE_CONFIG_VALID_PHASE_HIGH      0x57
#define VL53_REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI     0x51
#define VL53_REG_PRE_RANGE_CONFIG_VCSEL_PERIOD          0x50
#define VL53_REG_MSRC_CONFIG_TIMEOUT_MACROP             0x46
#define VL53_REG_IDENTIFICATION_MODEL_ID                0xC0
#define VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0       0xB0
#define VL53_REG_GLOBAL_CONFIG_REF_EN_START_SELECT      0xB6
#define VL53_REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD    0x4E
#define VL53_REG_DYNAMIC_SPAD_REF_EN_START_OFFSET       0x4F
#define VL53_REG_GLOBAL_CONFIG_VCSEL_WIDTH              0x32
#define VL53_REG_OSC_CALIBRATE_VAL                      0xF8
#define VL53_REG_ALGO_PHASECAL_LIM                      0x30
#define VL53_REG_ALGO_PHASECAL_CONFIG_TIMEOUT           0x30

#define VL53_MODEL_ID                                   0xEE
#define VL53_INIT_DELAY_MS                              100
#define VL53_BOOT_RETRIES                               8
#define VL53_BOOT_RETRY_DELAY_MS                        50
#define VL53_RANGING_TIMEOUT_MS                         250
#define VL53_READ_INTERVAL_MS                           1000
#define VL53_MAX_VALID_MM                               2000
#define VL53_EMA_ALPHA                                  0.20f
#define VL53_I2C_SPEED_HZ                               I2C_FREQ_HZ
#define VL53_MISSING_RETRY_MS                           3000
#define VL53_CONTINUOUS_PERIOD_MS                       50
#define VL53_RESTART_AFTER_FAILS                        5
#define VL53_INVALID_GRACE_READS                        3

#define decodeVcselPeriod(reg_val)      (((reg_val) + 1U) << 1U)
#define encodeVcselPeriod(period_pclks) (((period_pclks) >> 1U) - 1U)
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

// ── ใช้ TCA9548A: sensor ทุกตัวอยู่ที่ address 0x29 แยกด้วย channel ──
typedef struct {
    uint8_t  channel;       // TCA9548A channel (0-5)
    uint8_t  address;       // VL53L0X I2C addr = 0x29 ทุกตัว
    bool     present;
    bool     filter_ready;
    uint8_t  stop_variable;
    uint8_t  read_fail_count;
    uint8_t  invalid_sample_count;
    uint32_t measurement_timing_budget_us;
    float    filtered_mm;
    i2c_master_dev_handle_t dev;
} vl53_sensor_t;

static vl53_sensor_t s_sensors[PILL_SENSOR_COUNT] = {
    { 0, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
    { 1, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
    { 2, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
    { 3, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
    { 4, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
    { 5, VL53L0X_DEFAULT_ADDR, false, false, 0, 0, 0, 0, 0.0f, NULL },
};
static TickType_t s_retry_after_ticks[PILL_SENSOR_COUNT] = {0};


typedef struct {
    uint8_t reg;
    uint8_t *buf;
    size_t len;
} vl53_read_ctx_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
} vl53_write_ctx_t;

static esp_err_t vl53_read_reg(uint8_t addr, uint8_t reg, uint8_t *value);

// ── TCA9548A: เลือก channel ก่อนทุกครั้งที่สื่อสารกับ sensor ──
// MUST be called while holding i2c_manager_lock()
static int s_vl53_current_idx = 0;  // set before any vl53_* I/O call
static esp_err_t vl53_select_channel_nolock(int idx)
{
    uint8_t val = (uint8_t)(1u << (uint8_t)s_sensors[idx].channel);
    return i2c_manager_write_nolock(ADDR_TCA9548A, &val, 1);
}

// VL53 now uses i2c_manager for all I2C transactions (no private handles)



// All VL53 writes/reads hold the i2c mutex for the full channel-select + data sequence
// Before calling, set s_vl53_current_idx to the sensor index
static bool vl53_wait_for_model_id(uint8_t addr, uint8_t expected_model_id, int retries, int retry_delay_ms)
{
    uint8_t model_id = 0;
    for (int attempt = 0; attempt < retries; ++attempt) {
        if (vl53_read_reg(addr, VL53_REG_IDENTIFICATION_MODEL_ID, &model_id) == ESP_OK &&
            model_id == expected_model_id) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    }
    return false;
}

static esp_err_t vl53_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_write_reg_nolock(addr, reg, &value, 1);
    i2c_manager_unlock();
    return ret;
}

static esp_err_t vl53_write_reg16(uint8_t addr, uint8_t reg, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_write_reg_nolock(addr, reg, buf, 2);
    i2c_manager_unlock();
    return ret;
}

static esp_err_t vl53_write_multi(uint8_t addr, uint8_t reg, const uint8_t *src, size_t count)
{
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_write_reg_nolock(addr, reg, src, count);
    i2c_manager_unlock();
    return ret;
}

static esp_err_t vl53_read_reg(uint8_t addr, uint8_t reg, uint8_t *value)
{
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_read_reg_nolock(addr, reg, value, 1);
    i2c_manager_unlock();
    return ret;
}

static esp_err_t vl53_read_reg16(uint8_t addr, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2] = {0};
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_read_reg_nolock(addr, reg, buf, 2);
    i2c_manager_unlock();
    if (ret == ESP_OK) *value = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return ret;
}

static esp_err_t vl53_read_multi(uint8_t addr, uint8_t reg, uint8_t *dst, size_t count)
{
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_read_reg_nolock(addr, reg, dst, count);
    i2c_manager_unlock();
    return ret;
}

static esp_err_t vl53_write_reg32(uint8_t addr, uint8_t reg, uint32_t value)
{
    uint8_t buf[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
    i2c_manager_lock();
    vl53_select_channel_nolock(s_vl53_current_idx);
    esp_err_t ret = i2c_manager_write_reg_nolock(addr, reg, buf, 4);
    i2c_manager_unlock();
    return ret;
}

static uint16_t vl53_decode_timeout(uint16_t reg_val)
{
    return (uint16_t)((reg_val & 0x00FFU) << (uint16_t)((reg_val & 0xFF00U) >> 8)) + 1U;
}

static uint16_t vl53_encode_timeout(uint32_t timeout_mclks)
{
    uint32_t ls_byte = 0;
    uint16_t ms_byte = 0;

    if (timeout_mclks == 0) {
        return 0;
    }

    ls_byte = timeout_mclks - 1U;
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

static bool vl53_wait_until(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t expected, int timeout_ms)
{
    int waited_ms = 0;
    uint8_t value = 0;

    while (waited_ms < timeout_ms) {
        if (vl53_read_reg(addr, reg, &value) == ESP_OK && (value & mask) == expected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    return false;
}

static bool vl53_set_signal_rate_limit(uint8_t addr, float limit_mcps)
{
    if (limit_mcps < 0.0f || limit_mcps > 511.99f) {
        return false;
    }

    uint16_t limit_fixed = (uint16_t)(limit_mcps * (1 << 7));
    return vl53_write_reg16(addr, VL53_REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE, limit_fixed) == ESP_OK;
}

static bool vl53_get_spad_info(uint8_t addr, uint8_t *count, bool *type_is_aperture)
{
    uint8_t tmp = 0;
    int waited_ms = 0;

    if (vl53_write_reg(addr, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x06) != ESP_OK) {
        return false;
    }

    if (vl53_read_reg(addr, 0x83, &tmp) != ESP_OK) {
        return false;
    }
    if (vl53_write_reg(addr, 0x83, tmp | 0x04) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x07) != ESP_OK ||
        vl53_write_reg(addr, 0x81, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x94, 0x6B) != ESP_OK ||
        vl53_write_reg(addr, 0x83, 0x00) != ESP_OK) {
        return false;
    }

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(addr, 0x83, &tmp) == ESP_OK && tmp != 0x00) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    if (tmp == 0x00 ||
        vl53_write_reg(addr, 0x83, 0x01) != ESP_OK ||
        vl53_read_reg(addr, 0x92, &tmp) != ESP_OK) {
        return false;
    }

    *count = tmp & 0x7F;
    *type_is_aperture = ((tmp >> 7) & 0x01U) != 0;

    return vl53_write_reg(addr, 0x81, 0x00) == ESP_OK &&
           vl53_write_reg(addr, 0xFF, 0x06) == ESP_OK &&
           vl53_read_reg(addr, 0x83, &tmp) == ESP_OK &&
           vl53_write_reg(addr, 0x83, tmp & ~0x04U) == ESP_OK &&
           vl53_write_reg(addr, 0xFF, 0x01) == ESP_OK &&
           vl53_write_reg(addr, 0x00, 0x01) == ESP_OK &&
           vl53_write_reg(addr, 0xFF, 0x00) == ESP_OK &&
           vl53_write_reg(addr, 0x80, 0x00) == ESP_OK;
}

static bool vl53_get_sequence_step_enables(uint8_t addr, vl53_sequence_step_enables_t *enables)
{
    uint8_t sequence_config = 0;
    if (vl53_read_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, &sequence_config) != ESP_OK) {
        return false;
    }

    enables->tcc = ((sequence_config >> 4) & 0x1U) != 0;
    enables->dss = ((sequence_config >> 3) & 0x1U) != 0;
    enables->msrc = ((sequence_config >> 2) & 0x1U) != 0;
    enables->pre_range = ((sequence_config >> 6) & 0x1U) != 0;
    enables->final_range = ((sequence_config >> 7) & 0x1U) != 0;
    return true;
}

static bool vl53_get_sequence_step_timeouts(uint8_t addr, const vl53_sequence_step_enables_t *enables, vl53_sequence_step_timeouts_t *timeouts)
{
    uint8_t reg8 = 0;
    uint16_t reg16 = 0;

    if (vl53_read_reg(addr, VL53_REG_PRE_RANGE_CONFIG_VCSEL_PERIOD, &reg8) != ESP_OK) {
        return false;
    }
    timeouts->pre_range_vcsel_period_pclks = decodeVcselPeriod(reg8);

    if (vl53_read_reg(addr, VL53_REG_MSRC_CONFIG_TIMEOUT_MACROP, &reg8) != ESP_OK) {
        return false;
    }
    timeouts->msrc_dss_tcc_mclks = reg8 + 1U;
    timeouts->msrc_dss_tcc_us = vl53_timeout_mclks_to_microseconds(
        timeouts->msrc_dss_tcc_mclks,
        (uint8_t)timeouts->pre_range_vcsel_period_pclks
    );

    if (vl53_read_reg16(addr, VL53_REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI, &reg16) != ESP_OK) {
        return false;
    }
    timeouts->pre_range_mclks = vl53_decode_timeout(reg16);
    timeouts->pre_range_us = vl53_timeout_mclks_to_microseconds(
        timeouts->pre_range_mclks,
        (uint8_t)timeouts->pre_range_vcsel_period_pclks
    );

    if (vl53_read_reg(addr, VL53_REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD, &reg8) != ESP_OK) {
        return false;
    }
    timeouts->final_range_vcsel_period_pclks = decodeVcselPeriod(reg8);

    if (vl53_read_reg16(addr, VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI, &reg16) != ESP_OK) {
        return false;
    }
    timeouts->final_range_mclks = vl53_decode_timeout(reg16);
    if (enables->pre_range) {
        timeouts->final_range_mclks -= timeouts->pre_range_mclks;
    }
    timeouts->final_range_us = vl53_timeout_mclks_to_microseconds(
        timeouts->final_range_mclks,
        (uint8_t)timeouts->final_range_vcsel_period_pclks
    );

    return true;
}

static bool vl53_get_measurement_timing_budget(uint8_t addr, vl53_sensor_t *sensor)
{
    static const uint16_t StartOverhead = 1910;
    static const uint16_t EndOverhead = 960;
    static const uint16_t MsrcOverhead = 660;
    static const uint16_t TccOverhead = 590;
    static const uint16_t DssOverhead = 690;
    static const uint16_t PreRangeOverhead = 660;
    static const uint16_t FinalRangeOverhead = 550;

    vl53_sequence_step_enables_t enables;
    vl53_sequence_step_timeouts_t timeouts;
    if (!vl53_get_sequence_step_enables(addr, &enables) ||
        !vl53_get_sequence_step_timeouts(addr, &enables, &timeouts)) {
        return false;
    }

    uint32_t budget_us = StartOverhead + EndOverhead;
    if (enables.tcc) {
        budget_us += timeouts.msrc_dss_tcc_us + TccOverhead;
    }
    if (enables.dss) {
        budget_us += 2U * (timeouts.msrc_dss_tcc_us + DssOverhead);
    } else if (enables.msrc) {
        budget_us += timeouts.msrc_dss_tcc_us + MsrcOverhead;
    }
    if (enables.pre_range) {
        budget_us += timeouts.pre_range_us + PreRangeOverhead;
    }
    if (enables.final_range) {
        budget_us += timeouts.final_range_us + FinalRangeOverhead;
    }

    sensor->measurement_timing_budget_us = budget_us;
    return true;
}

static bool vl53_set_measurement_timing_budget(uint8_t addr, vl53_sensor_t *sensor, uint32_t budget_us)
{
    static const uint16_t StartOverhead = 1910;
    static const uint16_t EndOverhead = 960;
    static const uint16_t MsrcOverhead = 660;
    static const uint16_t TccOverhead = 590;
    static const uint16_t DssOverhead = 690;
    static const uint16_t PreRangeOverhead = 660;
    static const uint16_t FinalRangeOverhead = 550;

    vl53_sequence_step_enables_t enables;
    vl53_sequence_step_timeouts_t timeouts;
    if (!vl53_get_sequence_step_enables(addr, &enables) ||
        !vl53_get_sequence_step_timeouts(addr, &enables, &timeouts)) {
        return false;
    }

    uint32_t used_budget_us = StartOverhead + EndOverhead;
    if (enables.tcc) {
        used_budget_us += timeouts.msrc_dss_tcc_us + TccOverhead;
    }
    if (enables.dss) {
        used_budget_us += 2U * (timeouts.msrc_dss_tcc_us + DssOverhead);
    } else if (enables.msrc) {
        used_budget_us += timeouts.msrc_dss_tcc_us + MsrcOverhead;
    }
    if (enables.pre_range) {
        used_budget_us += timeouts.pre_range_us + PreRangeOverhead;
    }
    if (!enables.final_range) {
        return false;
    }

    used_budget_us += FinalRangeOverhead;
    if (used_budget_us > budget_us) {
        return false;
    }

    uint32_t final_range_timeout_us = budget_us - used_budget_us;
    uint32_t final_range_timeout_mclks = vl53_timeout_microseconds_to_mclks(
        final_range_timeout_us,
        (uint8_t)timeouts.final_range_vcsel_period_pclks
    );
    if (enables.pre_range) {
        final_range_timeout_mclks += timeouts.pre_range_mclks;
    }

    if (vl53_write_reg16(addr, VL53_REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
                         vl53_encode_timeout(final_range_timeout_mclks)) != ESP_OK) {
        return false;
    }

    sensor->measurement_timing_budget_us = budget_us;
    return true;
}

static bool vl53_perform_single_ref_calibration(uint8_t addr, uint8_t vhv_init_byte)
{
    uint8_t status = 0;
    int waited_ms = 0;

    if (vl53_write_reg(addr, VL53_REG_SYSRANGE_START, (uint8_t)(0x01 | vhv_init_byte)) != ESP_OK) {
        return false;
    }

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(addr, VL53_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07U) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    if ((status & 0x07U) == 0) {
        return false;
    }

    return vl53_write_reg(addr, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) == ESP_OK &&
           vl53_write_reg(addr, VL53_REG_SYSRANGE_START, 0x00) == ESP_OK;
}

static bool vl53_read_range_continuous_mm(uint8_t addr, uint16_t *range_mm)
{
    int waited_ms = 0;
    uint8_t status = 0;

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(addr, VL53_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07U) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    if ((status & 0x07U) == 0) {
        return false;
    }

    if (vl53_read_reg16(addr, (uint8_t)(VL53_REG_RESULT_RANGE_STATUS + 10U), range_mm) != ESP_OK) {
        return false;
    }

    return vl53_write_reg(addr, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) == ESP_OK;
}

static bool vl53_start_continuous(vl53_sensor_t *sensor, uint32_t period_ms)
{
    uint8_t addr = sensor->address;
    uint16_t osc_calibrate_val = 0;

    if (vl53_write_reg(addr, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x91, sensor->stop_variable) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x80, 0x00) != ESP_OK) {
        return false;
    }

    if (period_ms != 0) {
        if (vl53_read_reg16(addr, VL53_REG_OSC_CALIBRATE_VAL, &osc_calibrate_val) != ESP_OK) {
            return false;
        }
        if (osc_calibrate_val != 0) {
            period_ms *= osc_calibrate_val;
        }
        if (vl53_write_reg32(addr, VL53_REG_SYSTEM_INTERMEASUREMENT_PERIOD, period_ms) != ESP_OK ||
            vl53_write_reg(addr, VL53_REG_SYSRANGE_START, 0x04) != ESP_OK) {
            return false;
        }
    } else {
        if (vl53_write_reg(addr, VL53_REG_SYSRANGE_START, 0x02) != ESP_OK) {
            return false;
        }
    }

    return true;
}

static bool vl53_read_single_mm(vl53_sensor_t *sensor, uint16_t *range_mm)
{
    uint8_t addr = sensor->address;
    uint8_t start = 0;
    int waited_ms = 0;

    if (vl53_write_reg(addr, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x91, sensor->stop_variable) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x80, 0x00) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_SYSRANGE_START, 0x01) != ESP_OK) {
        return false;
    }

    while (waited_ms < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(addr, VL53_REG_SYSRANGE_START, &start) == ESP_OK &&
            (start & 0x01U) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }

    if ((start & 0x01U) != 0) {
        return false;
    }

    return vl53_read_range_continuous_mm(addr, range_mm);
}

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

static void vl53_release_sensor_handle(int idx)
{
    (void)idx;  // No private handles anymore, i2c_manager owns them
}

static void vl53_release_default_handle(void)
{
    // No-op: i2c_manager owns the 0x29 handle
}

static void vl53_mark_sensor_missing(int idx)
{
    vl53_release_sensor_handle(idx);
    s_sensors[idx].present = false;
    s_sensors[idx].filter_ready = false;
    s_sensors[idx].read_fail_count = 0;
    s_sensors[idx].invalid_sample_count = 0;
    s_sensors[idx].measurement_timing_budget_us = 0;
    s_sensors[idx].filtered_mm = 0.0f;
    tca9548a_disable_all(); // ปิด channel เมื่อ sensor หาย
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

static bool vl53_init_device(uint8_t addr, vl53_sensor_t *sensor)
{
    uint8_t model_id = 0;
    if (vl53_read_reg(addr, VL53_REG_IDENTIFICATION_MODEL_ID, &model_id) != ESP_OK || model_id != VL53_MODEL_ID) {
        ESP_LOGW(TAG, "VL53 model id mismatch at 0x%02X (read 0x%02X)", addr, model_id);
        return false;
    }

    uint8_t tmp = 0;
    if (vl53_read_reg(addr, VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, &tmp) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, tmp | 0x01U) != ESP_OK ||
        vl53_write_reg(addr, 0x88, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x00) != ESP_OK) {
        return false;
    }

    if (vl53_read_reg(addr, 0x91, &sensor->stop_variable) != ESP_OK ||
        vl53_write_reg(addr, 0x00, 0x01) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(addr, 0x80, 0x00) != ESP_OK ||
        vl53_read_reg(addr, VL53_REG_MSRC_CONFIG_CONTROL, &tmp) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_MSRC_CONFIG_CONTROL, tmp | 0x12U) != ESP_OK ||
        !vl53_set_signal_rate_limit(addr, 0.25f) ||
        vl53_write_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xFF) != ESP_OK) {
        return false;
    }

    uint8_t spad_count = 0;
    bool spad_type_is_aperture = false;
    if (!vl53_get_spad_info(addr, &spad_count, &spad_type_is_aperture)) {
        ESP_LOGW(TAG, "Failed to get SPAD info at 0x%02X", addr);
        return false;
    }

    uint8_t ref_spad_map[6] = {0};
    if (vl53_read_multi(addr, VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, sizeof(ref_spad_map)) != ESP_OK) {
        return false;
    }

    if (vl53_write_reg(addr, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C) != ESP_OK ||
        vl53_write_reg(addr, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4) != ESP_OK) {
        return false;
    }

    uint8_t first_spad_to_enable = spad_type_is_aperture ? 12U : 0U;
    uint8_t spads_enabled = 0;
    for (uint8_t i = 0; i < 48; i++) {
        if (i < first_spad_to_enable || spads_enabled == spad_count) {
            ref_spad_map[i / 8] &= (uint8_t)~(1U << (i % 8));
        } else if (((ref_spad_map[i / 8] >> (i % 8)) & 0x1U) != 0) {
            spads_enabled++;
        }
    }
    if (vl53_write_multi(addr, VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, sizeof(ref_spad_map)) != ESP_OK) {
        return false;
    }

    const struct { uint8_t reg; uint8_t value; } tuning[] = {
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
        if (vl53_write_reg(addr, tuning[i].reg, tuning[i].value) != ESP_OK) {
            ESP_LOGW(TAG, "Tuning write failed at 0x%02X reg 0x%02X", addr, tuning[i].reg);
            return false;
        }
    }

    if (vl53_write_reg(addr, VL53_REG_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04) != ESP_OK ||
        vl53_read_reg(addr, VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH, &tmp) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_GPIO_HV_MUX_ACTIVE_HIGH, tmp & (uint8_t)~0x10U) != ESP_OK ||
        vl53_write_reg(addr, VL53_REG_SYSTEM_INTERRUPT_CLEAR, 0x01) != ESP_OK) {
        return false;
    }

    if (!vl53_get_measurement_timing_budget(addr, sensor)) {
        return false;
    }

    if (vl53_write_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xE8) != ESP_OK ||
        !vl53_set_measurement_timing_budget(addr, sensor, sensor->measurement_timing_budget_us) ||
        vl53_write_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x01) != ESP_OK ||
        !vl53_perform_single_ref_calibration(addr, 0x40) ||
        vl53_write_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x02) != ESP_OK ||
        !vl53_perform_single_ref_calibration(addr, 0x00) ||
        vl53_write_reg(addr, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xE8) != ESP_OK) {
        return false;
    }

    return true;
}

// ── TCA9548A version: เลือก channel → init sensor ที่ 0x29 ──
static bool vl53_init_on_channel(int idx)
{
    s_vl53_current_idx = idx;  // All vl53_* I/O will select this channel atomically
    vl53_release_sensor_handle(idx);

    // Verify TCA9548A channel is reachable via a test write
    uint8_t ch_mask = (uint8_t)(1u << s_sensors[idx].channel);
    if (i2c_manager_write(ADDR_TCA9548A, &ch_mask, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Ch%d: TCA9548A select failed", idx);
        vl53_mark_sensor_missing(idx);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!vl53_wait_for_model_id(VL53L0X_DEFAULT_ADDR, VL53_MODEL_ID, VL53_BOOT_RETRIES, VL53_BOOT_RETRY_DELAY_MS)) {
        ESP_LOGW(TAG, "Ch%d: VL53 not found at 0x%02X", idx, VL53L0X_DEFAULT_ADDR);
        tca9548a_disable_all();
        vl53_mark_sensor_missing(idx);
        return false;
    }

    if (!vl53_init_device(VL53L0X_DEFAULT_ADDR, &s_sensors[idx])) {
        ESP_LOGW(TAG, "Ch%d: init failed", idx);
        tca9548a_disable_all();
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vl53_mark_sensor_present(idx);

    if (!vl53_start_continuous(&s_sensors[idx], VL53_CONTINUOUS_PERIOD_MS)) {
        ESP_LOGW(TAG, "Ch%d: continuous ranging start failed", idx);
        tca9548a_disable_all();
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(60));

    uint16_t raw_mm = 0;
    if (!vl53_read_range_continuous_mm(VL53L0X_DEFAULT_ADDR, &raw_mm)) {
        ESP_LOGW(TAG, "Ch%d: no first sample", idx);
        pill_sensor_status_set_reading(idx, -1, -1, false);
        tca9548a_disable_all();
        return true;
    }

    pill_sensor_status_set_reading(idx, (int)raw_mm, vl53_apply_filter(idx, raw_mm), true);
    ESP_LOGI(TAG, "Ch%d: ready, first=%u mm", idx, raw_mm);
    tca9548a_disable_all();
    return true;
}

void vl53l0x_multi_prepare_pins(void)
{
    // TCA9548A version: ไม่ต้องใช้ XSHUT pins
    // disable all channels ตอน init
    tca9548a_disable_all();
}

static void vl53_init_all(void)
{
    pill_sensor_status_init_defaults();
    tca9548a_disable_all();
    vl53_release_default_handle();
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        vl53_release_sensor_handle(i);
    }
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
            TickType_t now = xTaskGetTickCount();
            if (s_retry_after_ticks[i] == 0 || now >= s_retry_after_ticks[i]) {
                ESP_LOGI(TAG, "Ch%d: retry probe", i);
                (void)vl53_init_on_channel(i);
            }
            continue;
        }

        s_vl53_current_idx = i;  // channel selected atomically inside each vl53_* call
        uint16_t raw_mm = 0;
        if (!vl53_read_range_continuous_mm(VL53L0X_DEFAULT_ADDR, &raw_mm)) {
            s_sensors[i].read_fail_count++;
            if (!s_sensors[i].filter_ready || s_sensors[i].read_fail_count >= VL53_INVALID_GRACE_READS) {
                pill_sensor_status_set_reading(i, -1, -1, false);
            }
            if (s_sensors[i].read_fail_count >= VL53_RESTART_AFTER_FAILS) {
                ESP_LOGW(TAG, "Ch%d: dropped after %d fails", i, VL53_RESTART_AFTER_FAILS);
                vl53_mark_sensor_missing(i);
            }
            tca9548a_disable_all();
            continue;
        }

        s_sensors[i].read_fail_count = 0;
        if (raw_mm == 0 || raw_mm > VL53_MAX_VALID_MM) {
            s_sensors[i].invalid_sample_count++;
            if (!s_sensors[i].filter_ready || s_sensors[i].invalid_sample_count >= VL53_INVALID_GRACE_READS) {
                pill_sensor_status_set_reading(i, (int)raw_mm, -1, false);
            }
            tca9548a_disable_all();
            continue;
        }

        s_sensors[i].invalid_sample_count = 0;
        pill_sensor_status_set_reading(i, (int)raw_mm, vl53_apply_filter(i, raw_mm), true);
        tca9548a_disable_all();
    }
}

static void vl53_task(void *arg)
{
    (void)arg;

    static int s_last_reported_count[PILL_SENSOR_COUNT] = {-1, -1, -1, -1, -1, -1};
    static int s_stable_count[PILL_SENSOR_COUNT] = {-1, -1, -1, -1, -1, -1};
    static int s_stable_ticks[PILL_SENSOR_COUNT] = {0, 0, 0, 0, 0, 0};

    ESP_LOGI(TAG, "Starting VL53L0X poll task");
    while (1) {
        vl53_poll_all();
        
        // Sync sensor pill count to netpie shadow count
        const netpie_shadow_t *shadow = netpie_get_shadow();
        if (shadow && shadow->loaded) {
            for (int i = 0; i < PILL_SENSOR_COUNT; i++) {
                const pill_sensor_status_t *s = pill_sensor_status_get(i);
                if (s && s->valid && s->pill_count >= 0) {
                    if (s->pill_count != s_stable_count[i]) {
                        s_stable_count[i] = s->pill_count;
                        s_stable_ticks[i] = 1;
                    } else {
                        s_stable_ticks[i]++;
                    }
                    
                    // 3 ticks = ~6 seconds of stability
                    if (s_stable_ticks[i] >= 3) {
                        if (shadow->med[i].count != s_stable_count[i]) {
                            ESP_LOGI(TAG, "Sensor %d sync: %d -> %d pills", i+1, shadow->med[i].count, s_stable_count[i]);
                            netpie_shadow_update_count(i + 1, s_stable_count[i]);
                        }
                        s_last_reported_count[i] = s_stable_count[i];
                    }
                } else {
                    s_stable_count[i] = -1;
                    s_stable_ticks[i] = 0;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VL53_READ_INTERVAL_MS));
    }
}

void vl53l0x_multi_bootstrap(void)
{
    ESP_LOGI(TAG, "Bootstrapping VL53L0X sensors via TCA9548A");
    vl53_init_all();
}

void vl53l0x_multi_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }

    started = true;
    if (xTaskCreate(vl53_task, "vl53_task", 6144, NULL, 5, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "Failed to create VL53 task");
    }
}

void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills)
{
    pill_sensor_status_set_config(ch, full_dist_mm, pill_height_mm, max_pills);
}
