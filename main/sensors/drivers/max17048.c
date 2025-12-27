/**
 * @file max17048.c
 * @brief MAX17048 Li+ Battery Fuel Gauge driver implementation
 */

#include "max17048.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAX17048";

/**
 * @brief Write 16-bit register
 */
static esp_err_t max17048_write_reg(max17048_t *device, uint8_t reg, uint16_t value) {
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    
    return i2c_master_transmit(device->dev_handle, data, sizeof(data), 1000);
}

/**
 * @brief Read 16-bit register
 */
static esp_err_t max17048_read_reg(max17048_t *device, uint8_t reg, uint16_t *value) {
    uint8_t data[2] = {0};
    
    esp_err_t ret = i2c_master_transmit_receive(device->dev_handle, &reg, 1, data, 2, 1000);
    if (ret == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    
    return ret;
}

/**
 * @brief Initialize MAX17048
 */
esp_err_t max17048_init(max17048_t *device, i2c_master_bus_handle_t bus_handle) {
    if (device == NULL || bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing MAX17048 battery monitor");

    device->bus_handle = bus_handle;

    // Create I2C device handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX17048_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &device->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Read chip version to verify communication
    uint16_t version = 0;
    ret = max17048_read_version(device, &version);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MAX17048 chip version: 0x%04X", version);
    } else {
        ESP_LOGW(TAG, "Failed to read chip version");
    }

    return ESP_OK;
}

/**
 * @brief Deinitialize MAX17048
 */
esp_err_t max17048_deinit(max17048_t *device) {
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (device->dev_handle != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(device->dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to remove I2C device: %s", esp_err_to_name(ret));
            return ret;
        }
        device->dev_handle = NULL;
    }

    return ESP_OK;
}

/**
 * @brief Read cell voltage
 */
esp_err_t max17048_read_voltage(max17048_t *device, float *voltage) {
    if (device == NULL || voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_voltage = 0;
    esp_err_t ret = max17048_read_reg(device, MAX17048_REG_VCELL, &raw_voltage);
    
    if (ret == ESP_OK) {
        // Convert to voltage: VCELL = 12-bit value * 78.125 µV
        *voltage = (float)raw_voltage * 78.125f / 1000000.0f;
        ESP_LOGD(TAG, "Battery voltage: %.3f V", *voltage);
    }
    
    return ret;
}

/**
 * @brief Read state of charge
 */
esp_err_t max17048_read_soc(max17048_t *device, float *soc) {
    if (device == NULL || soc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_soc = 0;
    esp_err_t ret = max17048_read_reg(device, MAX17048_REG_SOC, &raw_soc);
    
    if (ret == ESP_OK) {
        // Convert to percentage: SOC = raw_value / 256
        *soc = (float)raw_soc / 256.0f;
        
        // Clamp to 0-100%
        if (*soc > 100.0f) *soc = 100.0f;
        if (*soc < 0.0f) *soc = 0.0f;
        
        ESP_LOGD(TAG, "Battery SOC: %.2f%%", *soc);
    }
    
    return ret;
}

/**
 * @brief Read chip version
 */
esp_err_t max17048_read_version(max17048_t *device, uint16_t *version) {
    if (device == NULL || version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return max17048_read_reg(device, MAX17048_REG_VERSION, version);
}

/**
 * @brief Reset the MAX17048 chip
 */
esp_err_t max17048_reset(max17048_t *device) {
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "Resetting MAX17048");
    
    esp_err_t ret = max17048_write_reg(device, MAX17048_REG_COMMAND, MAX17048_CMD_RESET);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for reset to complete
    }
    
    return ret;
}
