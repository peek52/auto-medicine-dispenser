#include "vl53l0x_multi.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_manager.h"
#include "pill_sensor_status.h"
#include "tca9548a.h"
#include "netpie_mqtt.h"
#include "telegram_bot.h"
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
/* Polling cadence — 5 s instead of 1 s. The pill cartridges fill at the
 * pace of "sometimes per day", so 1 Hz polling was 60x more bus traffic
 * than necessary and was directly contending with FT6336U + camera SCCB
 * + dispenser IR @ 2 kHz. Code paths that need an immediate fresh
 * reading (after dispense, after return-pill, after user adjusts count)
 * call vl53l0x_request_refresh() to force one poll cycle right away. */
#define VL53_READ_INTERVAL_MS                           5000
#define VL53_MAX_VALID_MM                               2000
// User config 2026-05-11: "want VL53 only to detect has-pill / empty,
// not precise counting". 30 s interval keeps the bus mostly free for
// touch / RTC while still updating empty status fast enough for the
// dispenser to flag a refill warning before the next dose.
/* Filter DISABLED (α = 1.0 → filtered_mm = raw_mm). Aligns the firmware
 * to the user's Arduino reference setup which reads raw values straight
 * from the chip with no smoothing and gets rock-steady results. Any
 * remaining instability is hardware-level (pullups, wire length, EMI)
 * not algorithmic, so no amount of filtering will help. */
#define VL53_EMA_ALPHA                                  1.00f
/* VL53L0X reads cleanly at 50 kHz even with long wires + no external
 * pullups (proven by the user's bare ESP32-S3 reference: VL53L0X x6
 * via TCA9548A reading rock-steady when Wire.setClock(50000)). The
 * shared bus runs at 400 kHz for everything else (touch, servo, RTC,
 * camera SCCB) — only the VL53 device handle uses 50 kHz here. */
#define VL53_I2C_SPEED_HZ                               50000
/* Retry interval for chips that didn't init (or got reset by camera SCCB).
 * Field-evidence (boot 2026-05-07): some marginal chips need a long XSHUT
 * pulse (500 ms LOW + 1.2 s HIGH) to wake — and the wake doesn't always
 * succeed on first try. Shortened from 300 s to 90 s so we catch a slowly-
 * warming chip within ~7 min instead of ~25 min. The 5-attempt progressive
 * pulse inside vl53_init_on_channel still bounds bus impact: each retry
 * probe is 5 attempts × <2 s = ~10 s of bus time per chip per 90 s, well
 * under the load that triggered the original "30 s flooded the bus"
 * regression (which had 5+ NACKs per attempt + uniformly short pulses). */
#define VL53_MISSING_RETRY_MS                           30000   // 30 s (was 90 s — bump retry frequency)
#define VL53_MAX_MISSING_RETRIES                        20   // 20 × 30 s ≈ 10 min recovery window
/* Layer-3 aggressive auto-retry: in the first 2 minutes after boot,
 * retry every 10 s instead of 30 s. This catches VL53 channels that
 * came up flaky during init (signal-integrity hiccups on the long
 * TCA-muxed wires) without making the user wait. After the early
 * phase we relax to the steady 30 s interval. */
#define VL53_EARLY_RETRY_MS                             10000   // 10 s
#define VL53_EARLY_RETRY_WINDOW_MS                      120000  // 2 min window
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

/* Cache the TCA channel that's currently selected so consecutive
 * vl53_io() calls to the same channel skip the redundant select+settle
 * burst. Reset to -1 by callers (vl53_io_begin_channel) when they want
 * to force a fresh select. */
static int s_tca_current_ch = -1;

/* Begin a multi-operation batch on `ch`: force a fresh TCA select +
 * the 10 ms analog-settle delay that the Arduino reference proves is
 * needed for stable reads. Subsequent vl53_io() calls in the same batch
 * will see s_tca_current_ch == ch and skip both. Must be called from a
 * task context that ALSO holds g_i2c_mutex — or set begin_locked=false
 * to take it briefly. */
static void vl53_io_begin_channel(int ch)
{
    s_tca_current_ch = -1;  /* force next vl53_io to re-select + settle */
}

/* End a batch: disable all TCA channels so other I2C clients (touch /
 * RTC / camera SCCB) see a clean upstream bus, and clear the cached
 * channel. Caller must still hold g_i2c_mutex on entry. */
static void vl53_io_end_channel_locked(void)
{
    (void)tca9548a_disable_all_locked();
    s_tca_current_ch = -1;
}

// Acquire mutex, select TCA channel, then run fn — all atomic under one lock.
// Prevents any other task from flipping the TCA channel mid-transaction.
static esp_err_t vl53_io(int ch, esp_err_t (*fn)(i2c_master_dev_handle_t dev, void *ctx), void *ctx)
{
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    /* Per-op retry with exponential backoff. VL53 init makes ~70
     * sequential I2C writes; with weak/long pull-ups + bus contention
     * the per-op success rate sits around 95%, which gave a compounded
     * init success of just ~3% (0.95^70). Going from 2 → 5 attempts
     * with backoff (200 µs / 1 ms / 3 ms / 8 ms) brings per-op success
     * to ~99.997% and full-init success to ~99.8%. */
    static const uint32_t kBackoffUs[5] = { 0, 200, 1000, 3000, 8000 };
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (kBackoffUs[attempt]) esp_rom_delay_us(kBackoffUs[attempt]);

        /* CRITICAL: only write to TCA + settle when we're switching
         * channels, not on every register access. Previously this
         * function fired the select + 10 ms delay on EVERY read of a
         * status register, totalling hundreds of milliseconds of dead
         * time per sensor read — and worse, it gave other I2C clients
         * (touch, NETPIE) a chance to interrupt the VL53 batch and
         * destabilize the mux state. The Arduino reference proves
         * one select per sensor is sufficient. */
        if (s_tca_current_ch != ch) {
            ret = tca9548a_select_channel_locked((uint8_t)ch);
            if (ret != ESP_OK) {
                s_tca_current_ch = -1;
                continue;
            }
            s_tca_current_ch = ch;
            /* Analog-settle delay AFTER the actual channel switch — the
             * mux's pass-gates need ~10 ms to propagate. */
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        ret = vl53_ensure_dev_locked();
        if (ret != ESP_OK) continue;

        ret = fn(s_vl53_dev, ctx);
        if (ret == ESP_OK) break;
    }

    // Drain any pending ISR before releasing the mutex so the next task
    // entering i2c_manager_* doesn't get blindsided by a stray VL53
    // transmit-completion ISR firing into its rx setup
    // (i2c_isr_receive_handler ptr=NULL panic).
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (bus) i2c_master_bus_wait_all_done(bus, 500);

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
    return i2c_master_transmit(dev, w->buf, w->len, 500);
}

static esp_err_t vl53_read_impl(i2c_master_dev_handle_t dev, void *ctx)
{
    vl53_read_ctx_t *r = (vl53_read_ctx_t *)ctx;
    esp_err_t ret = i2c_master_transmit(dev, &r->reg, 1, 500);
    if (ret != ESP_OK) return ret;
    i2c_master_bus_handle_t bus = i2c_manager_get_bus_handle();
    if (bus) i2c_master_bus_wait_all_done(bus, 500);
    return i2c_master_receive(dev, r->buf, r->len, 500);
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

/* Single-shot range read — port of Pololu Arduino library's
 * readRangeSingleMillimeters(). The user's bare ESP32-S3 + Pololu
 * VL53L0X demo (which works rock-steady) uses this exact sequence:
 *  1. Magic "start single ranging" register burst (stop_variable
 *     restored, range engine kicked).
 *  2. SYSRANGE_START = 0x01 to fire one measurement.
 *  3. Poll SYSRANGE_START bit 0 until clear (chip has accepted).
 *  4. Poll RESULT_INTERRUPT_STATUS until ranging interrupt fires.
 *  5. Read RESULT_RANGE_STATUS + 10 (16-bit mm).
 *  6. Clear the interrupt.
 * No state is carried between calls (vs continuous mode), so a stuck
 * chip can't return a stale value from a previous measurement. */
static bool vl53_read_range_single_mm(int ch, uint8_t stop_variable, uint16_t *range_mm)
{
    if (vl53_write_reg(ch, 0x80, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0x00, 0x00) != ESP_OK ||
        vl53_write_reg(ch, 0x91, stop_variable) != ESP_OK ||
        vl53_write_reg(ch, 0x00, 0x01) != ESP_OK ||
        vl53_write_reg(ch, 0xFF, 0x00) != ESP_OK ||
        vl53_write_reg(ch, 0x80, 0x00) != ESP_OK) return false;

    if (vl53_write_reg(ch, VL53_REG_SYSRANGE_START, 0x01) != ESP_OK) return false;

    /* Wait for SYSRANGE_START bit 0 to clear — chip has accepted. */
    int waited = 0;
    uint8_t sr = 0;
    while (waited < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(ch, VL53_REG_SYSRANGE_START, &sr) != ESP_OK) return false;
        if ((sr & 0x01) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
    if ((sr & 0x01) != 0) return false;

    /* Wait for ranging-complete interrupt. */
    uint8_t status = 0;
    waited = 0;
    while (waited < VL53_RANGING_TIMEOUT_MS) {
        if (vl53_read_reg(ch, VL53_REG_RESULT_INTERRUPT_STATUS, &status) == ESP_OK &&
            (status & 0x07U) != 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
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

/* Forward declarations for the NVS SPAD cache helpers (definitions live
 * at the bottom of this file with the other NVS calibration code).
 * The typedef is needed in full because vl53_init_device reads its
 * fields directly. */
typedef struct {
    uint8_t  magic;
    uint8_t  spad_count;
    uint8_t  spad_type_aperture;
    uint8_t  stop_variable;
    uint8_t  ref_spad_map[6];
} vl53_spad_blob_t;
#define VL53_SPAD_BLOB_MAGIC 0xA5
static bool vl53_spad_load_nvs(int ch, vl53_spad_blob_t *out);
static void vl53_spad_save_nvs(int ch, uint8_t spad_count, bool aperture,
                               uint8_t stop_var, const uint8_t map[6]);
static void vl53_spad_clear_nvs(int ch);

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

    /* Try to skip the timing-sensitive SPAD register reads by loading
     * a previously-cached calibration from NVS. Once any boot has
     * successfully initialised this channel we save its SPAD info,
     * then every subsequent boot can use the cached values instead of
     * re-reading them from the chip — bypassing the read step that
     * fails most often on weak-pull-up buses.
     *
     * VALIDATE the cache before using it. A boot can fail mid-init and
     * leave a junk save in NVS (e.g. stop=0x00 + count=0). Don't trust
     * a cache where spad_count is 0 (no SPAD enabled = nonsensical) —
     * fall through to fresh read in that case. */
    vl53_spad_blob_t spad_cache;
    bool spad_cached = vl53_spad_load_nvs(ch, &spad_cache);
    if (spad_cached && spad_cache.spad_count == 0) {
        ESP_LOGW(TAG, "Ch%d: SPAD cache REJECTED (count=0, will re-read)", ch);
        spad_cached = false;
    }

    if (spad_cached) {
        /* Use cached stop_variable instead of reading 0x91. */
        sensor->stop_variable = spad_cache.stop_variable;
        ESP_LOGI(TAG, "Ch%d: SPAD cache HIT (count=%u aperture=%u stop=0x%02X)",
                 ch, spad_cache.spad_count, spad_cache.spad_type_aperture,
                 spad_cache.stop_variable);
    } else {
        if (vl53_read_reg(ch, 0x91, &sensor->stop_variable) != ESP_OK) LOG_FAIL("read 0x91 stop_var");
    }
    if (vl53_write_reg(ch, 0x00, 0x01) != ESP_OK) LOG_FAIL("write 0x00 B");
    if (vl53_write_reg(ch, 0xFF, 0x00) != ESP_OK) LOG_FAIL("write 0xFF B");
    if (vl53_write_reg(ch, 0x80, 0x00) != ESP_OK) LOG_FAIL("write 0x80 B");
    if (vl53_read_reg(ch, VL53_REG_MSRC_CONFIG_CONTROL, &tmp) != ESP_OK) LOG_FAIL("read MSRC_CFG");
    if (vl53_write_reg(ch, VL53_REG_MSRC_CONFIG_CONTROL, tmp | 0x12U) != ESP_OK) LOG_FAIL("write MSRC_CFG");
    if (!vl53_set_signal_rate_limit(ch, 0.25f)) LOG_FAIL("set_signal_rate_limit");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xFF) != ESP_OK) LOG_FAIL("write SEQ_CFG 0xFF");

    uint8_t spad_count = 0;
    bool spad_type_is_aperture = false;
    uint8_t ref_spad_map[6] = {0};

    if (spad_cached) {
        /* Cache hit — skip both timing-critical SPAD reads entirely. */
        spad_count = spad_cache.spad_count;
        spad_type_is_aperture = (spad_cache.spad_type_aperture != 0);
        memcpy(ref_spad_map, spad_cache.ref_spad_map, sizeof(ref_spad_map));
    } else {
        /* Quiet the bus before the timing-critical SPAD read. Without
         * this the burst of register writes immediately preceding
         * leaves the line ringing, and the slow rising edges (weak
         * internal pull-ups) sometimes misread the SPAD count
         * register. Empirically a 20 ms gap cuts the per-attempt
         * failure rate from ~30% to <10%. */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!vl53_get_spad_info(ch, &spad_count, &spad_type_is_aperture)) LOG_FAIL("SPAD info");

        vTaskDelay(pdMS_TO_TICKS(20));  /* gap before next critical read */

        if (vl53_read_multi(ch, VL53_REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, sizeof(ref_spad_map)) != ESP_OK) LOG_FAIL("read SPAD map");
    }

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
    /* 100 ms timing budget — matches the user's working Arduino
     * reference (sensor[i].setMeasurementTimingBudget(100000) gave
     * rock-steady readings on a bare ESP32-S3 + TCA9548A + VL53L0X x6
     * with no external pullups). 100 ms is "high accuracy" per the
     * ST application note: the chip averages many internal returns
     * before delivering a single sample, so the raw value is already
     * clean before our median + EMA stages even see it. */
    sensor->measurement_timing_budget_us = 100000;
    if (!vl53_set_measurement_timing_budget(ch, sensor, sensor->measurement_timing_budget_us)) LOG_FAIL("set_timing_budget");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x01) != ESP_OK) LOG_FAIL("write SEQ 0x01");

    vTaskDelay(pdMS_TO_TICKS(20));  /* settle before ref_cal (timing-critical) */

    if (!vl53_perform_single_ref_calibration(ch, 0x40)) LOG_FAIL("ref_cal 0x40");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0x02) != ESP_OK) LOG_FAIL("write SEQ 0x02");

    vTaskDelay(pdMS_TO_TICKS(20));  /* settle between ref_cal steps */

    if (!vl53_perform_single_ref_calibration(ch, 0x00)) LOG_FAIL("ref_cal 0x00");
    if (vl53_write_reg(ch, VL53_REG_SYSTEM_SEQUENCE_CONFIG, 0xE8) != ESP_OK) LOG_FAIL("write SEQ 0xE8 B");

    /* Save SPAD calibration on first successful init for this channel.
     * Subsequent boots will use the cached values instead of re-reading
     * from the chip, skipping the most failure-prone step entirely.
     * Skip the save (and skip the SAVED log message) when the read
     * values are obviously bogus — vl53_spad_save_nvs itself also
     * guards against this, but logging only here avoids the misleading
     * "NOT saving ... SAVED" double-message in the serial monitor. */
    if (!spad_cached && spad_count > 0) {
        vl53_spad_save_nvs(ch, spad_count, spad_type_is_aperture,
                           sensor->stop_variable, ref_spad_map);
        ESP_LOGI(TAG, "Ch%d: SPAD cache SAVED (count=%u aperture=%d stop=0x%02X)",
                 ch, spad_count, (int)spad_type_is_aperture, sensor->stop_variable);
    }
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

/* Layer-3 helper: pick retry interval based on uptime. During the
 * first VL53_EARLY_RETRY_WINDOW_MS after boot we want aggressive
 * recovery (10 s) so any channel that hiccuped during init gets
 * 12 fast attempts. After that, relax to the steady 30 s. */
static uint32_t vl53_retry_interval_ms(void)
{
    const uint32_t uptime_ms =
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return (uptime_ms < VL53_EARLY_RETRY_WINDOW_MS)
               ? VL53_EARLY_RETRY_MS
               : VL53_MISSING_RETRY_MS;
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
    s_retry_after_ticks[idx] = xTaskGetTickCount() + pdMS_TO_TICKS(vl53_retry_interval_ms());
}

static void vl53_mark_sensor_present(int idx)
{
    s_sensors[idx].present = true;
    s_sensors[idx].read_fail_count = 0;
    s_sensors[idx].invalid_sample_count = 0;
    s_retry_after_ticks[idx] = 0;
    pill_sensor_status_mark_present(idx, true);
}

/* Hard-reset a single channel's VL53L0X by driving its XSHUT line
 * low, waiting, then releasing it. Field-evidence (boot 2026-05-07):
 * Ch2 chip refused to wake on every standard 30 ms LOW + 150 ms HIGH
 * pulse, then suddenly came up clean on a retry-probe ~15 min later
 * — meaning the chip IS healthy but needs much longer reset/wake time
 * than the datasheet minimum on this specific board. Solution: scale
 * pulse duration with retry attempt — short pulse first (cheap), and
 * if that doesn't wake the chip we extend BOTH the LOW (full state-
 * machine drain) and HIGH (full internal cal) windows progressively.
 * attempt indices 1..4 map to {short, medium, long, very-long}. */
static void vl53_xshut_pulse(int idx, int attempt)
{
#if !VL53L0X_XSHUT_PRESENT
    /* No physical XSHUT wiring — recovery via XSHUT is impossible. */
    (void)idx; (void)attempt;
    return;
#else
    static const gpio_num_t xshut[PILL_SENSOR_COUNT] = {
        VL53L0X_XSHUT_M1, VL53L0X_XSHUT_M2, VL53L0X_XSHUT_M3,
        VL53L0X_XSHUT_M4, VL53L0X_XSHUT_M5, VL53L0X_XSHUT_M6,
    };
    if (idx < 0 || idx >= PILL_SENSOR_COUNT || xshut[idx] < 0) return;

    /* Progressive durations. Index 0 (= bootstrap pre-wake) handled
     * separately by vl53_xshut_release_one(); we don't reach this
     * function for attempt 0. Beyond attempt 4 we cap at the longest. */
    static const uint32_t low_ms[]  = { 30,  80, 200, 500 };
    static const uint32_t high_ms[] = { 150, 300, 600, 1200 };
    int slot = attempt - 1;
    if (slot < 0) slot = 0;
    if (slot >= (int)(sizeof(low_ms) / sizeof(low_ms[0]))) {
        slot = (int)(sizeof(low_ms) / sizeof(low_ms[0])) - 1;
    }

    gpio_set_level(xshut[idx], 0);
    vTaskDelay(pdMS_TO_TICKS(low_ms[slot]));
    gpio_set_level(xshut[idx], 1);
    vTaskDelay(pdMS_TO_TICKS(high_ms[slot]));
#endif
}

static bool vl53_init_on_channel(int idx)
{
    /* Outer retry — bumped from 3 to 5 attempts because field testing
     * shows some channels (esp. Ch3 with longer wires) need extra
     * cycles to come up cleanly. Each retry now also runs a longer
     * XSHUT pulse (see vl53_xshut_pulse) and probes the model-ID with
     * extended retry budget. */
    bool inited = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Ch%d: hard-reset retry %d/5 via XSHUT (progressive pulse)", idx, attempt + 1);
            vl53_xshut_pulse(idx, attempt);
        }
        if (!vl53_wait_for_model_id(idx)) {
            ESP_LOGW(TAG, "Ch%d: VL53 not found at 0x%02X (attempt %d)",
                     idx, VL53L0X_DEFAULT_ADDR, attempt + 1);
            continue;
        }
        if (vl53_init_device(idx, &s_sensors[idx])) {
            inited = true;
            break;
        }
        ESP_LOGW(TAG, "Ch%d: init_device failed on attempt %d", idx, attempt + 1);
        /* On the 2nd attempt, invalidate any cached SPAD blob — if a
         * previous boot saved bad data the cache could keep us stuck.
         * Clearing forces the next attempt to do a fresh read. */
        if (attempt == 1) {
            vl53_spad_clear_nvs(idx);
        }
    }
    if (!inited) {
        ESP_LOGW(TAG, "Ch%d: init failed after 5 attempts", idx);
        vl53_mark_sensor_missing(idx);
        return false;
    }

    vl53_mark_sensor_present(idx);

    /* Single-shot read mode — Pololu-style. We DON'T start continuous
     * ranging at init time; each vl53_poll_all iteration fires its own
     * single shot via vl53_read_range_single_mm(). The chip's
     * stop_variable is captured by vl53_init_device and replayed on
     * every single-shot read. Simpler state machine, no risk of stale
     * data from a previous measurement. */
    vTaskDelay(pdMS_TO_TICKS(60));

    /* Single-shot first read — proves the chip is responsive at the
     * application level. Uses the same code path as vl53_poll_all to
     * surface any single-shot-specific issue at init time rather than
     * later in production. */
    uint16_t raw_mm = 0;
    vl53_io_begin_channel(idx);
    bool first_ok = vl53_read_range_single_mm(idx, s_sensors[idx].stop_variable, &raw_mm);
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    vl53_io_end_channel_locked();
    xSemaphoreGive(g_i2c_mutex);
    if (!first_ok) {
        ESP_LOGW(TAG, "Ch%d: no first single-shot sample (read fail)", idx);
        pill_sensor_status_set_reading(idx, -1, -1, false);
        return true;
    }

    pill_sensor_status_set_reading(idx, (int)raw_mm, (int)raw_mm, true);
    ESP_LOGI(TAG, "Ch%d: ready, first=%u mm", idx, raw_mm);
    return true;
}

/* Configure XSHUT GPIOs as outputs and HOLD ALL CHIPS IN RESET (LOW).
 * Previous code drove all 6 HIGH simultaneously which caused all chips
 * to start their boot/calibration current draw at the exact same moment.
 * On boards with marginal 3.3 V capacitance / pull-ups that produced a
 * rail dip deep enough that some chips brown-out partway through their
 * internal cal → random "Ch_i not found at 0x29" failures that swap
 * around between Ch3/Ch4/Ch5 each cold boot.
 *
 * Now we keep every chip OFF here, then bring them up one at a time in
 * vl53_init_all() — only ONE chip pulls boot-current at any moment, so
 * the rail surge is bounded and reproducible regardless of board lot.
 */
static void vl53_setup_xshut_gpios(void)
{
#if !VL53L0X_XSHUT_PRESENT
    /* Pins not wired — every chip is left ON by its module-board's
     * internal pullup on XSHUT. Skip GPIO setup entirely; the TCA9548A
     * mux + per-channel init is sufficient. */
    ESP_LOGI(TAG, "XSHUT pins not wired (config) — chips assumed always ON");
    return;
#else
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
        if (xshut[i] >= 0) gpio_set_level(xshut[i], 0);
    }
#endif
}

/* Bring a single VL53L0X out of reset. Datasheet requires ≥1.2 ms after
 * XSHUT release before I2C is usable; we wait 200 ms so the chip's
 * internal cal completes even on under-decoupled boards. */
static void vl53_xshut_release_one(int idx)
{
#if !VL53L0X_XSHUT_PRESENT
    /* No-op — chips are already on. Still settle briefly so I2C bus is
     * stable before the next init step runs (matches the timing the
     * GPIO-wired path uses). */
    (void)idx;
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
#else
    static const gpio_num_t xshut[PILL_SENSOR_COUNT] = {
        VL53L0X_XSHUT_M1, VL53L0X_XSHUT_M2, VL53L0X_XSHUT_M3,
        VL53L0X_XSHUT_M4, VL53L0X_XSHUT_M5, VL53L0X_XSHUT_M6,
    };
    if (idx < 0 || idx >= PILL_SENSOR_COUNT || xshut[idx] < 0) return;
    gpio_set_level(xshut[idx], 1);
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
}

static void vl53_init_all(void)
{
    pill_sensor_status_init_defaults();
    /* Configure XSHUTs and HOLD ALL chips in reset. We will release them
     * one at a time below to stagger the boot-current surge. */
    vl53_setup_xshut_gpios();
    tca9548a_disable_all();
    vl53_release_dev();
    /* Brief settle after we've forced every chip OFF — lets the 3.3 V
     * rail recover from any prior in-rush before we start bringing chips
     * online. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Per-channel: release ONE chip from reset, init it, move on.
     * Only one chip is ever in the high-current "boot + cal + init
     * write burst" window at a time, so the shared 3.3 V rail never
     * sees a 6× surge. This makes init success deterministic instead
     * of "whichever chip happens to brown-out latest fails at random". */
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        vl53_xshut_release_one(i);
        (void)vl53_init_on_channel(i);
        /* Small inter-channel gap so the just-finished chip's TCA
         * channel state fully drains before we flip to the next. */
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void vl53_poll_all(void)
{
    /* Stagger: at most ONE retry per poll cycle. VL53 init takes
     * ~1-2 s and holds the I2C bus the entire time. If we retried 6
     * channels in a single 5 s cycle the touch / IR / display would
     * be locked out for the better part of 10 s. By retrying one
     * channel per cycle the bus stays responsive (still ~10-15 s for
     * all 6 channels to get a retry attempt, which is plenty). */
    bool retried_one_this_cycle = false;
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        if (!s_sensors[i].present) {
            if (s_sensors[i].permanently_missing) continue;
            if (retried_one_this_cycle) continue;
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
                retried_one_this_cycle = true;
                if (vl53_init_on_channel(i)) {
                    s_sensors[i].missing_retry_count = 0;
                } else {
                    /* Schedule next retry using the interval appropriate
                     * for current uptime (10 s during early window,
                     * 30 s after). */
                    s_retry_after_ticks[i] = now + pdMS_TO_TICKS(vl53_retry_interval_ms());
                }
            }
            continue;
        }

        /* Single-shot read — exact mirror of Arduino reference code.
         * The previous median-of-5 + EMA stack was over-engineered for
         * a sensor that the same chip on a bare ESP32-S3 reads cleanly
         * without ANY filtering. One TCA select per channel, one
         * single-shot read, raw value out. */
        vl53_io_begin_channel(i);
        uint16_t raw_mm = 0;
        bool got_sample = vl53_read_range_single_mm(
            i, s_sensors[i].stop_variable, &raw_mm);
        /* Free the mux for other I2C clients before moving to next ch. */
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        vl53_io_end_channel_locked();
        xSemaphoreGive(g_i2c_mutex);

        if (!got_sample) {
            s_sensors[i].read_fail_count++;
            if (!s_sensors[i].filter_ready || s_sensors[i].read_fail_count >= VL53_INVALID_GRACE_READS) {
                // Preserve the last raw value via _invalidate() so the
                // diagnostic web UI can still show the last sample
                // before the sensor went stale (helps the user spot a
                // sensor that fails intermittently vs one that's
                // completely dead).
                pill_sensor_status_invalidate(i);
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
                // Out-of-range read (0 = too close, >2000 = no target).
                // Update raw_mm to the actual reading so the UI shows
                // "ดิบ 2047mm" — that tells the user "sensor sees the
                // back of an empty cartridge" rather than "no read".
                pill_sensor_status_set_reading(i, (int)raw_mm, -1, false);
            }
            continue;
        }

        s_sensors[i].invalid_sample_count = 0;
        pill_sensor_status_set_reading(i, (int)raw_mm, vl53_apply_filter(i, raw_mm), true);
    }
}

/* Allow other subsystems (e.g. camera_init's SCCB burst) to temporarily
 * silence the VL53 poll loop so the I2C bus is fully quiet. Without this,
 * concurrent VL53 retry probes contend with SCCB and the OV5647 PID read
 * NACKs from accumulated bus noise. Set non-zero to pause; clear to resume. */
static volatile int s_vl53_pause_count = 0;

/* Set true while the polling task is actively in vl53_poll_all() — i.e.
 * holding the I2C bus and TCA channel state. Cleared while waiting on
 * the inter-poll delay or while observing pause. Lets camera_init wait
 * for the task to actually quiesce instead of just hoping a fixed delay
 * is long enough. */
static volatile bool s_vl53_io_busy = false;

/* Set true while vl53l0x_multi_bootstrap() is running. Camera retry task
 * checks this and waits before pulling all VL53 XSHUT pins LOW for its
 * SCCB burst — without coordination, the camera retry yanks XSHUT mid-
 * bootstrap, which corrupts every VL53 init that hadn't completed yet
 * (Ch3+ in field logs). */
static volatile bool s_vl53_bootstrapping = false;

bool vl53l0x_multi_is_bootstrapping(void)
{
    return s_vl53_bootstrapping;
}

void vl53l0x_multi_pause(void)
{
    /* Atomic increment is fine — only set from non-ISR contexts. */
    s_vl53_pause_count++;
}

void vl53l0x_multi_resume(void)
{
    if (s_vl53_pause_count > 0) s_vl53_pause_count--;
}

/* Block until the VL53 task observes the pause flag and is no longer in
 * vl53_poll_all(), or until timeout_ms elapses. Caller must have already
 * called vl53l0x_multi_pause(). Used by camera_init() to ensure the
 * shared I2C bus is genuinely quiet before issuing the SCCB burst that
 * detects + configures the OV5647. Without this, the camera SCCB writes
 * race against in-flight VL53 register reads and the sensor PID NACKs
 * (= "Camera attempt N failed" looping all 16 retries). */
void vl53l0x_multi_wait_idle(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_vl53_io_busy) {
        if (xTaskGetTickCount() >= deadline) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* On-demand refresh — wakes the VL53 task to run ONE poll cycle.
 *
 * Architecture change 2026-05-12: the auto-polling loop is GONE — VL53
 * stays idle except when something explicitly asks for a fresh read.
 * That's almost always the Telegram /status command (see
 * vl53l0x_multi_check_now). The old 30 s polling caused bus contention
 * with touch / IR / RTC that the user was experiencing as flaky reads
 * and panics; reads are now bus-quiet by default. */
static SemaphoreHandle_t s_check_request_sem  = NULL;   /* given by trigger, taken by task */
static SemaphoreHandle_t s_check_complete_sem = NULL;   /* given by task, taken by waiter */

void vl53l0x_request_refresh(void)
{
    if (s_check_request_sem) {
        xSemaphoreGive(s_check_request_sem);
    }
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        s_retry_after_ticks[i] = 0;
        s_sensors[i].missing_retry_count = 0;
    }
}

static void vl53_task(void *arg)
{
    (void)arg;

    /* Request-driven task. Sits on s_check_request_sem indefinitely.
     * Each give wakes the task to run ONE poll cycle (all channels in
     * sequence, ~3 s total). The old auto-poll-every-30 s loop is gone
     * because it caused bus contention with touch / IR during dispense
     * and was triggering false-positive flaps in the dashboard.
     *
     * Trigger sources (2026-05-12):
     *   - Telegram /status command → vl53l0x_multi_check_now (blocking)
     *   - Dispense post-event   → vl53l0x_request_refresh (fire-and-forget)
     *
     * VL53 readings are NO LONGER published to NETPIE — user dropped the
     * dashboard tiles. Telegram /status formats a fresh reading inline. */
    ESP_LOGI(TAG, "VL53L0X task ready (request-driven, no auto-poll)");
    while (1) {
        /* Block until something asks for a check. portMAX_DELAY = bus
         * stays quiet forever unless explicitly woken. */
        if (s_check_request_sem) {
            xSemaphoreTake(s_check_request_sem, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Honor pause requests from camera_init etc. — if paused, drop
         * this request (caller can retry once camera is done). */
        if (s_vl53_pause_count > 0) {
            ESP_LOGW(TAG, "VL53 check request received during pause — skipping");
            if (s_check_complete_sem) xSemaphoreGive(s_check_complete_sem);
            continue;
        }

        /* Lazy bootstrap. Init storm (~10-15 s, 6 × 70 I2C writes) was
         * running at boot and hogging the bus enough to make touch feel
         * unresponsive immediately after startup. Deferring init until
         * the first /status request means boot finishes clean — touch
         * works right away — at the cost of a longer first /status. */
        static bool s_bootstrapped = false;
        if (!s_bootstrapped) {
            ESP_LOGI(TAG, "VL53 lazy bootstrap (first check request)");
            s_vl53_bootstrapping = true;
            vl53_init_all();
            s_vl53_bootstrapping = false;
            s_bootstrapped = true;
        }

        s_vl53_io_busy = true;
        vl53_poll_all();
        s_vl53_io_busy = false;

        /* DEBUG: dump all-6 distance every poll. Remove once tuning done. */
        {
            char line[160];
            int n = snprintf(line, sizeof(line), "vl53 raw[mm]:");
            for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
                const pill_sensor_status_t *s = pill_sensor_status_get(i);
                if (!s_sensors[i].present) {
                    n += snprintf(line + n, sizeof(line) - n, " M%d=---", i + 1);
                } else if (s && s->valid) {
                    n += snprintf(line + n, sizeof(line) - n,
                                  " M%d=%d(f%d)", i + 1, s->raw_mm, s->filtered_mm);
                } else {
                    n += snprintf(line + n, sizeof(line) - n, " M%d=??", i + 1);
                }
                if (n >= (int)sizeof(line)) break;
            }
            ESP_LOGI(TAG, "%s", line);
        }

        /* VL53 distances / pill counts are NO LONGER published to NETPIE
         * (2026-05-12). User dropped the dashboard tiles to keep this
         * sensor on-demand only. The values are still available via
         * pill_sensor_status_get() for the Telegram /status command. */
        int pills_pub[6];
        for (int i = 0; i < 6; ++i) {
            const pill_sensor_status_t *s = pill_sensor_status_get(i);
            bool valid_now = (s && s_sensors[i].present && s->valid && s->pill_count >= 0);
            pills_pub[i] = valid_now ? s->pill_count : -1;
        }

        /* Low-pill Telegram alert — edge trigger when count crosses the
         * threshold from above (was > VL53_LOW_PILL_ALERT, now ≤), with a
         * per-channel cooldown so we don't spam if the count hovers at
         * the boundary. Re-arms when the count climbs above threshold + 1
         * (refill detected). */
        static int  s_last_alert_pills[6] = { -2, -2, -2, -2, -2, -2 };
        static uint32_t s_last_alert_ms[6] = { 0 };
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint32_t ALERT_COOLDOWN_MS = 30 * 60 * 1000;  /* 30 min */
        for (int i = 0; i < 6; ++i) {
            int p = pills_pub[i];
            if (p < 0) continue;
            int prev = s_last_alert_pills[i];
            bool refilled = (prev >= 0 && prev <= VL53_LOW_PILL_ALERT && p > VL53_LOW_PILL_ALERT + 1);
            bool first_low = (p <= VL53_LOW_PILL_ALERT) &&
                             (prev == -2 || prev > VL53_LOW_PILL_ALERT);
            bool cooled    = (now_ms - s_last_alert_ms[i]) >= ALERT_COOLDOWN_MS;
            bool repeat_low = (p <= VL53_LOW_PILL_ALERT) && cooled;
            if (refilled) {
                s_last_alert_pills[i] = p;
            } else if (first_low || repeat_low) {
                const netpie_shadow_t *sh = netpie_get_shadow();
                const char *name = (sh && sh->med[i].name[0]) ? sh->med[i].name : "";
                char msg[160];
                if (p == 0) {
                    snprintf(msg, sizeof(msg),
                             "⚠️ ตลับที่ %d%s%s — ยาหมดแล้ว",
                             i + 1, name[0] ? " " : "", name);
                } else {
                    /* User asked to drop the numeric pill count from
                     * Telegram (sensor isn't trusted as a counter, just
                     * as a fill-level indicator). Keep it short: "low". */
                    snprintf(msg, sizeof(msg),
                             "⚠️ ตลับที่ %d%s%s — ยาใกล้หมด กรุณาเติมยา",
                             i + 1, name[0] ? " " : "", name);
                }
                telegram_send_text(msg);
                s_last_alert_pills[i] = p;
                s_last_alert_ms[i]    = now_ms;
            } else {
                s_last_alert_pills[i] = p;
            }
        }

        /* Signal completion so any blocking caller (e.g. /status
         * Telegram handler waiting in vl53l0x_multi_check_now) can
         * collect the freshly-read values. The next iteration goes
         * straight back to xSemaphoreTake — task sleeps with zero CPU
         * until the next trigger. */
        if (s_check_complete_sem) xSemaphoreGive(s_check_complete_sem);
    }
}

// ─── Public API ─────────────────────────────────────────────────────────────

void vl53l0x_multi_bootstrap(void)
{
    ESP_LOGI(TAG, "Bootstrapping VL53L0X sensors via TCA9548A");
    s_vl53_bootstrapping = true;
    vl53_init_all();
    s_vl53_bootstrapping = false;
}

void vl53l0x_multi_start(void)
{
    static bool started = false;
    if (started) return;

    /* Lazy-create the request/complete semaphores before spawning the
     * task — the task references them on first xSemaphoreTake. */
    if (!s_check_request_sem)  s_check_request_sem  = xSemaphoreCreateBinary();
    if (!s_check_complete_sem) s_check_complete_sem = xSemaphoreCreateBinary();
    if (!s_check_request_sem || !s_check_complete_sem) {
        ESP_LOGE(TAG, "Failed to create VL53 check semaphores");
        return;
    }

    started = true;
    if (xTaskCreate(vl53_task, "vl53_task", 8192, NULL, 5, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "Failed to create VL53 task");
    }
}

/* Blocking on-demand check. Triggers one full poll cycle (all six
 * channels through the TCA mux) and waits for completion. Returns
 * ESP_OK if the task signaled done within timeout, ESP_ERR_TIMEOUT
 * otherwise (e.g. paused for camera SCCB). After return, the values
 * are available via pill_sensor_status_get(idx). */
esp_err_t vl53l0x_multi_check_now(uint32_t timeout_ms)
{
    if (!s_check_request_sem || !s_check_complete_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Drain any stale completion signal so we only collect the result
     * of THE poll we're about to trigger. */
    xSemaphoreTake(s_check_complete_sem, 0);

    /* Reset retry timers so any sensor in cool-down gets re-probed. */
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        s_retry_after_ticks[i] = 0;
        s_sensors[i].missing_retry_count = 0;
    }

    xSemaphoreGive(s_check_request_sem);
    if (xSemaphoreTake(s_check_complete_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

// Persist per-channel calibration so it survives reboots. Key format:
//   namespace "vl53_cal", key "ch0".."ch5", blob = 3 × int16_t = full_dist_mm,
//   pill_height_mm, max_pills. Old defaults are restored if no blob exists.
typedef struct {
    int16_t full_dist_mm;
    int16_t pill_height_mm;
    int16_t max_pills;
} vl53_cal_blob_t;

/* SPAD calibration values: typedef + magic defined near vl53_init_device
 * (forward-declaration at top of file). Caching these in NVS lets
 * subsequent boots skip the timing-critical SPAD register reads. */

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

static void vl53_offset_save_nvs(int ch, int offset)
{
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[10];
    snprintf(key, sizeof(key), "off%d", ch);
    nvs_set_i16(h, key, (int16_t)offset);
    nvs_commit(h);
    nvs_close(h);
}

/* SPAD cache helpers. Stored per-channel under key "spad%d". */
static bool vl53_spad_load_nvs(int ch, vl53_spad_blob_t *out)
{
    if (!out) return false;
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[10];
    snprintf(key, sizeof(key), "spad%d", ch);
    size_t sz = sizeof(*out);
    bool ok = (nvs_get_blob(h, key, out, &sz) == ESP_OK && sz == sizeof(*out) &&
               out->magic == VL53_SPAD_BLOB_MAGIC);
    nvs_close(h);
    return ok;
}

static void vl53_spad_save_nvs(int ch, uint8_t spad_count, bool aperture,
                               uint8_t stop_var, const uint8_t map[6])
{
    /* Validate before saving. spad_count==0 means the chip returned
     * an invalid SPAD configuration (often happens when a sensor is
     * physically dead or disconnected). Saving this poisons the cache
     * for future boots and forces repeated bad init attempts. */
    if (spad_count == 0) {
        ESP_LOGW(TAG, "Ch%d: NOT saving SPAD cache (count=0, sensor likely dead)", ch);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[10];
    snprintf(key, sizeof(key), "spad%d", ch);
    vl53_spad_blob_t blob = {
        .magic = VL53_SPAD_BLOB_MAGIC,
        .spad_count = spad_count,
        .spad_type_aperture = aperture ? 1u : 0u,
        .stop_variable = stop_var,
    };
    memcpy(blob.ref_spad_map, map, 6);
    nvs_set_blob(h, key, &blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}

static void vl53_spad_clear_nvs(int ch)
{
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[10];
    snprintf(key, sizeof(key), "spad%d", ch);
    if (nvs_erase_key(h, key) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGW(TAG, "Ch%d: cleared bad SPAD cache, next retry will re-read", ch);
    }
    nvs_close(h);
}

void vl53l0x_load_calibration_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(VL53_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int ch = 0; ch < PILL_SENSOR_COUNT; ++ch) {
        char key[10];
        snprintf(key, sizeof(key), "ch%d", ch);
        vl53_cal_blob_t blob;
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, key, &blob, &sz) == ESP_OK && sz == sizeof(blob) &&
            blob.full_dist_mm > 0 && blob.pill_height_mm > 0 && blob.max_pills > 0) {
            pill_sensor_status_set_config(ch, blob.full_dist_mm, blob.pill_height_mm, blob.max_pills);
            ESP_LOGI(TAG, "Ch%d cal loaded from NVS: full=%dmm height=%dmm max=%d",
                     ch, blob.full_dist_mm, blob.pill_height_mm, blob.max_pills);
        }
        snprintf(key, sizeof(key), "off%d", ch);
        int16_t off = 0;
        if (nvs_get_i16(h, key, &off) == ESP_OK) {
            pill_sensor_status_set_offset(ch, off);
            if (off != 0) ESP_LOGI(TAG, "Ch%d count_offset=%+d (NVS)", ch, off);
        }
    }
    nvs_close(h);
}

void vl53l0x_set_channel_config(int ch, int full_dist_mm, int pill_height_mm, int max_pills)
{
    pill_sensor_status_set_config(ch, full_dist_mm, pill_height_mm, max_pills);
    vl53_cal_save_nvs(ch, full_dist_mm, pill_height_mm, max_pills);
}

void vl53l0x_set_channel_offset(int ch, int count_offset)
{
    pill_sensor_status_set_offset(ch, count_offset);
    vl53_offset_save_nvs(ch, count_offset);
}
