/**
 * @file i2c_scanner.h
 * @brief I2C bus scanner for detecting connected devices
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C Configuration (Adafruit Metro ESP32-S3 STEMMA QT connector)
#define I2C_MASTER_SCL_IO           48      /*!< GPIO number for I2C master clock */
#define I2C_MASTER_SDA_IO           47      /*!< GPIO number for I2C master data  */
#define I2C_MASTER_FREQ_HZ          100000  /*!< I2C master clock frequency */
#define I2C_MASTER_TIMEOUT_MS       1000    /*!< I2C timeout */

/**
 * @brief Initialize I2C master bus
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t i2c_scanner_init(void);

/**
 * @brief Scan I2C bus for devices
 * 
 * Scans all addresses from 0x08 to 0x77 and reports found devices
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t i2c_scanner_scan(void);

/**
 * @brief Deinitialize I2C master bus
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t i2c_scanner_deinit(void);

/**
 * @brief Check if a device exists at the given I2C address
 * 
 * @param address I2C address to check (7-bit)
 * @return true if device responds
 * @return false if no device at address
 */
bool i2c_scanner_device_exists(uint8_t address);

/**
 * @brief Get the I2C bus handle
 * 
 * @return i2c_master_bus_handle_t I2C bus handle (NULL if not initialized)
 */
i2c_master_bus_handle_t i2c_scanner_get_bus_handle(void);

#ifdef __cplusplus
}
#endif
