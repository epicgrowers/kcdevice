/**
 * @file max17048.h
 * @brief MAX17048 Li+ Battery Fuel Gauge driver for ESP32
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// MAX17048 I2C address
#define MAX17048_I2C_ADDR       0x36

// MAX17048 Register addresses
#define MAX17048_REG_VCELL      0x02    // Cell voltage
#define MAX17048_REG_SOC        0x04    // State of charge
#define MAX17048_REG_MODE       0x06    // Mode register
#define MAX17048_REG_VERSION    0x08    // Chip version
#define MAX17048_REG_CONFIG     0x0C    // Configuration
#define MAX17048_REG_COMMAND    0xFE    // Command register

// Commands
#define MAX17048_CMD_RESET      0x5400  // Power-on reset

/**
 * @brief MAX17048 handle structure
 */
typedef struct {
    i2c_master_bus_handle_t bus_handle;     // I2C bus handle
    i2c_master_dev_handle_t dev_handle;     // I2C device handle
} max17048_t;

/**
 * @brief Initialize MAX17048 battery monitor
 * 
 * @param device Pointer to MAX17048 handle
 * @param bus_handle I2C bus handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_init(max17048_t *device, i2c_master_bus_handle_t bus_handle);

/**
 * @brief Deinitialize MAX17048 battery monitor
 * 
 * @param device Pointer to MAX17048 handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_deinit(max17048_t *device);

/**
 * @brief Read cell voltage
 * 
 * @param device Pointer to MAX17048 handle
 * @param voltage Pointer to store voltage in volts
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_read_voltage(max17048_t *device, float *voltage);

/**
 * @brief Read state of charge (battery percentage)
 * 
 * @param device Pointer to MAX17048 handle
 * @param soc Pointer to store state of charge (0-100%)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_read_soc(max17048_t *device, float *soc);

/**
 * @brief Read chip version
 * 
 * @param device Pointer to MAX17048 handle
 * @param version Pointer to store chip version
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_read_version(max17048_t *device, uint16_t *version);

/**
 * @brief Reset the MAX17048 chip
 * 
 * @param device Pointer to MAX17048 handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t max17048_reset(max17048_t *device);

#ifdef __cplusplus
}
#endif
