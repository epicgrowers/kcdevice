/**
 * @file i2c_scanner.c
 * @brief I2C bus scanner implementation
 */

#include "i2c_scanner.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "I2C_SCAN";

static i2c_master_bus_handle_t bus_handle = NULL;

esp_err_t i2c_scanner_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C master bus");
    ESP_LOGI(TAG, "  SDA: GPIO%d, SCL: GPIO%d", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C master bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "✓ I2C master bus initialized successfully");
    return ESP_OK;
}

bool i2c_scanner_device_exists(uint8_t address)
{
    if (bus_handle == NULL) {
        return false;
    }
    
    // Try to probe the device
    esp_err_t ret = i2c_master_probe(bus_handle, address, I2C_MASTER_TIMEOUT_MS);
    return (ret == ESP_OK);
}

esp_err_t i2c_scanner_scan(void)
{
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized. Call i2c_scanner_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Scanning I2C bus for devices...");
    ESP_LOGI(TAG, "========================================");
    
    uint8_t devices_found = 0;
    
    // Scan all valid I2C addresses (0x08 to 0x77)
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t ret = i2c_master_probe(bus_handle, addr, I2C_MASTER_TIMEOUT_MS);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ Device found at address 0x%02X", addr);
            devices_found++;
            
            // Print common device names for known addresses
            switch (addr) {
                case 0x1E: ESP_LOGI(TAG, "  → Possible: HMC5883L (Magnetometer)"); break;
                case 0x20:
                case 0x21:
                case 0x22:
                case 0x23:
                case 0x24:
                case 0x25:
                case 0x26:
                case 0x27: ESP_LOGI(TAG, "  → Possible: PCF8574 (I/O Expander) or LCD"); break;
                case 0x38: ESP_LOGI(TAG, "  → Possible: FT6236 (Touch Controller)"); break;
                case 0x39: ESP_LOGI(TAG, "  → Possible: TSL2561/APDS9960 (Light Sensor)"); break;
                case 0x3C:
                case 0x3D: ESP_LOGI(TAG, "  → Possible: SSD1306 (OLED Display)"); break;
                case 0x40: ESP_LOGI(TAG, "  → Possible: PCA9685/SI7021 (PWM/Humidity)"); break;
                case 0x48:
                case 0x49:
                case 0x4A:
                case 0x4B: ESP_LOGI(TAG, "  → Possible: ADS1115/PCF8591 (ADC)"); break;
                case 0x50:
                case 0x51:
                case 0x52:
                case 0x53:
                case 0x54:
                case 0x55:
                case 0x56:
                case 0x57: ESP_LOGI(TAG, "  → Possible: AT24C (EEPROM)"); break;
                case 0x68:
                case 0x69: ESP_LOGI(TAG, "  → Possible: MPU6050/DS3231/DS1307 (IMU/RTC)"); break;
                case 0x76:
                case 0x77: ESP_LOGI(TAG, "  → Possible: BME280/BMP280 (Temp/Pressure/Humidity)"); break;
                default: break;
            }
        }
    }
    
    ESP_LOGI(TAG, "========================================");
    if (devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices found!");
        ESP_LOGW(TAG, "Check wiring and pull-up resistors");
    } else {
        ESP_LOGI(TAG, "Scan complete: %d device(s) found", devices_found);
    }
    ESP_LOGI(TAG, "========================================");
    
    return ESP_OK;
}

esp_err_t i2c_scanner_deinit(void)
{
    if (bus_handle != NULL) {
        esp_err_t ret = i2c_del_master_bus(bus_handle);
        if (ret == ESP_OK) {
            bus_handle = NULL;
            ESP_LOGI(TAG, "I2C master bus deinitialized");
        }
        return ret;
    }
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_scanner_get_bus_handle(void)
{
    return bus_handle;
}
