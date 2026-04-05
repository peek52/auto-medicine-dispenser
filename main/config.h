#pragma once

// ─────────────────────────────────────────────
//  config.h — Unified pin & address definitions
//  ESP32-P4-NANO  (unified_cam project)
// ─────────────────────────────────────────────

// ── WiFi Credentials (overridden by NVS if saved) ──
#define WIFI_SSID_DEFAULT   "PJ_Main router_true"
#define WIFI_PASS_DEFAULT   "pajaroen"

// ── I2C Bus (I2C_NUM_0) ────────────────────────────
// Shared by: Camera sensor (SCCB), PCF8574, PCA9685, DS3231, FT6336U
#define I2C_SDA_PIN         7
#define I2C_SCL_PIN         8
#define I2C_FREQ_HZ         50000   // 50 kHz — required for FT6336U stability

// ── I2C Device Addresses ──────────────────────────
#define ADDR_PCF8574        0x20    // IR sensor (PCF8574) — confirmed by I2C scan
#define ADDR_PCA9685        0x40    // Servo driver (PCA9685)
#define ADDR_DS3231         0x68    // RTC (DS3231)
#define ADDR_EEPROM         0x56    // EEPROM on RTC module (optional)
#define ADDR_FT6336U        0x38    // Touch controller (FT6336U) — needs CTP_RST init before I2C responds
#define CTP_RST_PIN         21      // FT6336U hardware reset (active-low pulse)
#define CTP_INT_PIN         -1      // FT6336U interrupt — not connected (-1 = unused)

// ── Camera (MIPI-CSI2 / OV5647) ──────────────────
#define CAM_LDO_CHAN_ID     3
#define CAM_LDO_VOLTAGE_MV  2500
#define CSI_HRES            800
#define CSI_VRES            640
#define CSI_FORMAT_NAME     "MIPI_2lane_24Minput_RAW8_800x640_50fps" // OV5647 stable mode
#define CAM_XCLK_PIN        33
#define CAM_XCLK_FREQ       24000000

// ── Servo (PCA9685) ─────────────────────────────
#define SERVO_NUM_CHANNELS  6
#define SERVO_FREQ_HZ       50
#define SERVO_MIN_PULSEWIDTH_US  500    // Pulse width for 0°
#define SERVO_MAX_PULSEWIDTH_US  2500   // Pulse width for 180°

// ── Web Server ──────────────────────────────────
#define WEB_SERVER_PORT     80

// ── TFT Display — ST7796S SPI (480×320) ─────────
#define TFT_MOSI    32
#define TFT_SCK     36
#define TFT_CS      26
#define TFT_DC      24
#define TFT_RST     25

// ── NETPIE 2020 (MQTT) ───────────────────────────
#define NETPIE_BROKER       "mqtt.netpie.io"
#define NETPIE_PORT         1883
#define NETPIE_CLIENT_ID    "1501dee1-63c2-4d4b-80a3-ff5cb7c14ab1"
#define NETPIE_TOKEN        "AUAbCxfYonbPQgBKyuGxwvas1vL9M9Md"
#define NETPIE_SECRET       "4mgegowymnSxszLKSqMiguD4z2Rxim8"
#define NETPIE_DEVICE       "esp32cam_dispenser"

// Shadow topics (NETPIE 2020 format)
#define NETPIE_TOPIC_GET    "@shadow/data/get"
#define NETPIE_TOPIC_RESP   "@shadow/data/get/response"
#define NETPIE_TOPIC_UPDATED "@shadow/data/updated"
#define NETPIE_TOPIC_SET    "@shadow/data/update"

// ── Dispenser ────────────────────────────────────
#define DISPENSER_MED_COUNT 6   // จำนวนตลับยา (servo channels 0-5)
#define DISPENSER_MAX_PILLS 16  // เม็ดยาสูงสุดต่อตลับ

// ── Telegram Bot ────────────────────────────────
// Telegram credentials are configured at runtime via /cloud and stored in NVS.
#define TELEGRAM_CHECK_INTERVAL 3000

// Cloud web access code (stored in NVS after first boot if unset)
#define CLOUD_ACCESS_CODE_DEFAULT "cloud2026"
#define TECH_ACCESS_CODE_DEFAULT  "tech2026"
#define ADMIN_ACCESS_CODE_DEFAULT "master2026"

// ── DFPlayer Mini ───────────────────────────────
#define DFPLAYER_TX_PIN       37      // ESP32-P4 UART0_TXD -> DY-T20L S2
#define DFPLAYER_RX_PIN       38      // ESP32-P4 UART0_RXD <- DY-T20L S1
#define DFPLAYER_UART_NUM     UART_NUM_1

// ── Google Sheets Log ───────────────────────────
// ใส่ URL ที่ได้จากการ Deploy Web App ของ Google Apps Script ที่ช่องนี้
// Google Script URL is configured at runtime via /cloud and stored in NVS.
