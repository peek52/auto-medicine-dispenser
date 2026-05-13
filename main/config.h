#pragma once

// Unified pin and address definitions for ESP32-P4-NANO.

// WiFi credentials are overridden by NVS after saving from the web UI.
#define WIFI_SSID_DEFAULT   "PJ_Main router_true"
#define WIFI_PASS_DEFAULT   "pajaroen"

// Optional peripherals.
// Re-enabled after the local IDF i2c_master patch (state-reset retry on
// INVALID_STATE). VL53 bootstrap no longer wedges the bus permanently
// because the patched i2c_master_transmit() retries with FSM reset.
#define ENABLE_VL53_PILL_SENSORS 1

// I2C bus: shared by camera SCCB, PCF8574, PCA9685, DS3231, FT6336U, and TCA9548A.
// Per-device clock speed is selected in i2c_manager.c (get_or_add_device).
// Field-tested split (user reports IR significantly more accurate at 400 kHz
// — shorter mutex hold → less interleaving with servo PWM noise window):
//   400 kHz — PCF8574, PCA9685, FT6336U, DS3231, TCA9548A, EEPROM
//   100 kHz — VL53L0X (via TCA mux, long wires), OV5647 SCCB
#define I2C_SDA_PIN         7
#define I2C_SCL_PIN         8
/* Base bus init speed. Per-device clock overrides this (see
 * device_clock_hz in i2c_manager.c). Lowered 400 kHz → 100 kHz
 * 2026-05-13 to make the entire system run at standard-mode I2C —
 * eliminates rise-time issues on the long shared bus that were
 * causing servo+IR+VL53 to fight each other when active concurrently. */
#define I2C_FREQ_HZ         100000

// VL53L0X multi-sensor bus through TCA9548A, with one XSHUT line per module.
#define VL53L0X_DEFAULT_ADDR 0x29
#define VL53L0X_ADDR_M1      0x71
#define VL53L0X_ADDR_M2      0x72
#define VL53L0X_ADDR_M3      0x73
#define VL53L0X_ADDR_M4      0x74
#define VL53L0X_ADDR_M5      0x75
#define VL53L0X_ADDR_M6      0x76

/* XSHUT physical wiring. Set VL53L0X_XSHUT_PRESENT to 0 when the chips'
 * XSHUT pins are LEFT FLOATING (or only tied to module-board pullup) —
 * the per-channel address dance becomes unnecessary because the TCA9548A
 * mux isolates each sensor anyway. With XSHUT absent:
 *   - The boot-time "hold all in reset" sequence is skipped (no-op
 *     writes to disconnected GPIOs were causing confusing log spam).
 *   - The mid-init "hard reset via XSHUT" retry is skipped — recovery
 *     for a wedged sensor requires a full board reboot.
 *   - camera_init's pre-SCCB XSHUT pulldown is skipped — pins aren't
 *     connected, so dropping VL53 off the bus during camera SCCB is
 *     not actually possible. (Bus contention is still mitigated by
 *     the i2c_manager mutex.) */
#define VL53L0X_XSHUT_PRESENT 0
#define VL53L0X_XSHUT_M1     20
#define VL53L0X_XSHUT_M2     22
#define VL53L0X_XSHUT_M3     23
#define VL53L0X_XSHUT_M4     47
#define VL53L0X_XSHUT_M5     48
#define VL53L0X_XSHUT_M6     53

// I2C device addresses.
#define ADDR_PCF8574        0x20    // IR sensor expander.
#define ADDR_PCA9685        0x40    // Servo driver.
#define ADDR_DS3231         0x68    // RTC.
#define ADDR_EEPROM         0x56    // Optional EEPROM on RTC module.
#define ADDR_FT6336U        0x38    // Touch controller.
#define ADDR_TCA9548A       0x70    // TCA9548A I2C multiplexer.
#define CTP_RST_PIN         21      // FT6336U hardware reset, active-low pulse.
#define CTP_INT_PIN         -1      // FT6336U interrupt is not connected.

// VL53L0X pill stock measurement.
//   tube length 260 mm, pill height 15 mm, capacity 15 pills
//   d ≈ 30 mm → full (15), d ≈ 255 mm → empty (0)
//   pill_count = max_pills - round((dist_mm - full_dist_mm) / pill_height_mm)
#define VL53_FULL_DIST_MM       30
#define VL53_PILL_HEIGHT_MM     15
#define VL53_MAX_PILLS          15
#define VL53_EMPTY_DIST_MM      (VL53_FULL_DIST_MM + VL53_PILL_HEIGHT_MM * VL53_MAX_PILLS)
#define VL53_LOW_PILL_ALERT     3   /* Telegram แจ้งเตือนเมื่อ ≤ ค่านี้ */

// Camera (MIPI-CSI2 / OV5647).
#define CAM_LDO_CHAN_ID     3
// 2800 mV (per OV5647 datasheet AVDD spec). 2500 mV worked on some
// boards but caused the sensor to drop SCCB register writes / report
// a corrupted PID (0x568f) on this unit.
#define CAM_LDO_VOLTAGE_MV  2800
#define CSI_HRES            800
#define CSI_VRES            640
#define CSI_FORMAT_NAME     "MIPI_2lane_24Minput_RAW8_800x640_50fps"
#define CAM_XCLK_PIN        33
#define CAM_XCLK_FREQ       24000000

// Servo (PCA9685).
#define SERVO_NUM_CHANNELS  6
#define SERVO_FREQ_HZ       50
#define SERVO_MIN_PULSEWIDTH_US  500
#define SERVO_MAX_PULSEWIDTH_US  2500

// Web server.
#define WEB_SERVER_PORT     80

// TFT display: ST7796S SPI, 480x320.
#define TFT_MOSI    32
#define TFT_SCK     36
#define TFT_CS      26
#define TFT_DC      24
#define TFT_RST     25

// SD card is currently disabled in firmware.
#define ENABLE_SD_CARD        0
#define SD_CARD_CLK_PIN       43
#define SD_CARD_CMD_PIN       44
#define SD_CARD_D0_PIN        39
#define SD_CARD_D1_PIN        40
#define SD_CARD_D2_PIN        41
#define SD_CARD_D3_PIN        42
#define SD_CARD_POWER_PIN     45
#define SD_CARD_MOUNT_POINT   "/sdcard"

// NETPIE 2020 MQTT.
#define NETPIE_BROKER       "mqtt.netpie.io"
#define NETPIE_PORT         1883
#define NETPIE_CLIENT_ID    "1501dee1-63c2-4d4b-80a3-ff5cb7c14ab1"
#define NETPIE_TOKEN        "AUAbCxfYonbPQgBKyuGxwvas1vL9M9Md"
#define NETPIE_SECRET       "4mgegowymnSxszLKSqMiguD4z2Rxim8"
#define NETPIE_DEVICE       "esp32cam_dispenser"

#define NETPIE_TOPIC_GET     "@shadow/data/get"
#define NETPIE_TOPIC_RESP    "@shadow/data/get/response"
#define NETPIE_TOPIC_UPDATED "@shadow/data/updated"
#define NETPIE_TOPIC_SET     "@shadow/data/update"

// Dispenser.
#define DISPENSER_MED_COUNT 6
#define DISPENSER_MAX_PILLS 16

// Telegram bot polling interval.
#define TELEGRAM_CHECK_INTERVAL 3000

// Cloud web access codes, stored in NVS after first boot if unset.
#define CLOUD_ACCESS_CODE_DEFAULT "cloud2026"
#define TECH_ACCESS_CODE_DEFAULT  "tech2026"
#define ADMIN_ACCESS_CODE_DEFAULT "master2026"

// DY-HV20T audio module, kept behind the historical dfplayer wrapper names.
#define DFPLAYER_TX_PIN       37
#define DFPLAYER_RX_PIN       38
#define DFPLAYER_UART_NUM     UART_NUM_1

// Google Apps Script URL is configured at runtime via /cloud and stored in NVS.
