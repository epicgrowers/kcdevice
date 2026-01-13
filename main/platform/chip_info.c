/**
 * @file chip_info.c
 * @brief Chip detection and information utilities implementation
 */

#include "chip_info.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include <string.h>

static const char *TAG = "PLATFORM:CHIP";

/**
 * @brief Get chip model name as string
 */
const char* chip_info_get_model_name(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    switch (chip_info.model) {
        case CHIP_ESP32:     return "ESP32";
        case CHIP_ESP32S2:   return "ESP32-S2";
        case CHIP_ESP32S3:   return "ESP32-S3";
        case CHIP_ESP32C3:   return "ESP32-C3";
        case CHIP_ESP32C2:   return "ESP32-C2";
        case CHIP_ESP32C6:   return "ESP32-C6";
        case CHIP_ESP32H2:   return "ESP32-H2";
        case CHIP_ESP32P4:   return "ESP32-P4";
        default:             return "Unknown";
    }
}

/**
 * @brief Check if running on ESP32-S3
 */
bool chip_info_is_esp32s3(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    return chip_info.model == CHIP_ESP32S3;
}

/**
 * @brief Check if running on ESP32-C6
 */
bool chip_info_is_esp32c6(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    return chip_info.model == CHIP_ESP32C6;
}

/**
 * @brief Get architecture name
 */
static const char* get_architecture_name(void)
{
#ifdef CONFIG_IDF_TARGET_ARCH_XTENSA
    return "Xtensa";
#elif defined(CONFIG_IDF_TARGET_ARCH_RISCV)
    return "RISC-V";
#else
    return "Unknown";
#endif
}

/**
 * @brief Log detailed chip information
 */
void chip_info_log(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Chip Information");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Model:         %s", chip_info_get_model_name());
    ESP_LOGI(TAG, "Architecture:  %s", get_architecture_name());
    ESP_LOGI(TAG, "Revision:      v%d.%d", chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "CPU Cores:     %d", chip_info.cores);
    ESP_LOGI(TAG, "Flash Size:    %lu MB", flash_size / (1024 * 1024));
    
    // Feature flags
    ESP_LOGI(TAG, "Features:");
    ESP_LOGI(TAG, "  - WiFi:      %s", (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "Yes (2.4 GHz)" : "No");
    ESP_LOGI(TAG, "  - Bluetooth: %s", (chip_info.features & CHIP_FEATURE_BT) ? "Yes (Classic)" : "No");
    ESP_LOGI(TAG, "  - BLE:       %s", (chip_info.features & CHIP_FEATURE_BLE) ? "Yes" : "No");
    
    // Check for PSRAM (works for both embedded and external PSRAM)
    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "  - PSRAM:     Yes (%lu MB)", psram_size / (1024 * 1024));
    } else {
        ESP_LOGI(TAG, "  - PSRAM:     No");
    }

#ifdef CONFIG_IDF_TARGET_ESP32C6
    ESP_LOGI(TAG, "  - WiFi 6:    Yes (802.11ax)");
    ESP_LOGI(TAG, "  - Zigbee:    Yes (802.15.4)");
    ESP_LOGI(TAG, "  - Thread:    Yes (802.15.4)");
#endif
    
    ESP_LOGI(TAG, "========================================");
    
    // Log IDF version
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "========================================");
}
