#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * i2c_manager — Shared I2C bus (I2C_NUM_0) with mutex protection.
 * All modules must take/give the mutex around each transaction.
 */

// Global mutex — take before any i2c master transaction, give after
extern SemaphoreHandle_t g_i2c_mutex;

/** Initialise I2C_NUM_0 at I2C_FREQ_HZ and create the mutex */
esp_err_t i2c_manager_init(void);

/** Get the shared I2C master bus handle */
i2c_master_bus_handle_t i2c_manager_get_bus_handle(void);

/**
 * Probe a device address. Returns ESP_OK if ACK received.
 * Acquires mutex internally.
 */
esp_err_t i2c_manager_ping(uint8_t addr);

/**
 * Write bytes to device. Acquires mutex internally.
 * @param addr     7-bit device address
 * @param data     bytes to send (includes register if needed)
 * @param len      number of bytes
 */
esp_err_t i2c_manager_write(uint8_t addr, const uint8_t *data, size_t len);

/**
 * Write a register byte followed by data bytes. Acquires mutex internally.
 * Equivalent to: [reg, data[0], data[1], ... data[len-1]]
 */
esp_err_t i2c_manager_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

/**
 * Write register byte then read reply. Acquires mutex internally.
 * @param addr     7-bit device address
 * @param reg      register byte to write first
 * @param buf      output buffer
 * @param len      number of bytes to read
 */
esp_err_t i2c_manager_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

/**
 * Pure read (no register byte sent). For devices like PCF8574.
 * Acquires mutex internally.
 */
esp_err_t i2c_manager_read(uint8_t addr, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
