#pragma once

// Unified pin and address definitions for ESP32-P4-NANO.

// WiFi credentials are overridden by NVS after saving from the web UI.
#define WIFI_SSID_DEFAULT   "PJ_Main router_true"
#define WIFI_PASS_DEFAULT   "pajaroen"

/* IR detection on direct GPIO. Each IR module's OUT line is read on
 * its own ESP32 GPIO with the internal pull-up enabled. Module is
 * active-LOW (OUT pulled LOW when the beam is blocked → indicator LED
 * ON). */
#define IR_GPIO_M1   20
#define IR_GPIO_M2   22
#define IR_GPIO_M3   23
#define IR_GPIO_M4   47
#define IR_GPIO_M5   48
/* M6 ย้าย GPIO 46 → GPIO 2 (2026-05-13). GPIO 46 อยู่ในกลุ่ม SD reserved
 * แต่ภาคสนามพบว่าไม่รับสัญญาณ digital จาก IR module (ทดสอบสลับสาย/โมดูล
 * แล้ว — pin นี้ใช้ไม่ได้). GPIO 2 เป็น general-purpose free pin
 * ทดสอบใช้งานได้จริง. */
#define IR_GPIO_M6   2

// I2C bus: shared by camera SCCB, PCA9685, DS3231, FT6336U.
#define I2C_SDA_PIN         7
#define I2C_SCL_PIN         8
#define I2C_FREQ_HZ         100000

// I2C device addresses.
#define ADDR_PCA9685        0x40    // Servo driver.
#define ADDR_DS3231         0x68    // RTC.
#define ADDR_EEPROM         0x56    // Optional EEPROM on RTC module.
#define ADDR_FT6336U        0x38    // Touch controller.
#define CTP_RST_PIN         21      // FT6336U hardware reset, active-low pulse.
#define CTP_INT_PIN         -1      // FT6336U interrupt is not connected.

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

// ── ESP32-C3 Super Mini distance-sensor bridge (READ-ONLY) ──
// External C3 carries the TCA9548A + 6 VL53L0X sensors and streams six
// distance readings out its UART. The P4 listens passively on UART2
// and exposes the latest values via /vl53 — it does NOT command the C3
// and does NOT feed values into dispense / IR detection / schedule.
// Wiring (C3 GPIO 4 = TX, C3 GPIO 5 = RX from c3_vl53_bridge.ino):
//   C3 TX (GPIO 4) ───► P4 RX (GPIO 53)
//   C3 RX (GPIO 5) ◄─── P4 TX (GPIO 54)   (reserved; not transmitted)
//   GND ↔ GND
// GPIO 53/54 chosen this time after the GPIO 32/36 attempt boot-hung
// — those pins are TFT_MOSI / TFT_SCK above. 53/54 are ADC2_CHANNEL
// 6/7 on the Waveshare P4-Nano right-side header with no other claim.
#define BRIDGE_RX_PIN         53
#define BRIDGE_TX_PIN         54
#define BRIDGE_UART_NUM       UART_NUM_2
#define BRIDGE_UART_BAUD      115200

// Google Apps Script URL is configured at runtime via /cloud and stored in NVS.
