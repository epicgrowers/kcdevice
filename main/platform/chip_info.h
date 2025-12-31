/**
 * @file chip_info.h
 * @brief Chip detection and information utilities
 * 
 * Provides runtime detection of ESP32 chip type and hardware capabilities
 */

#pragma once

#include "esp_system.h"
#include "esp_chip_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log detailed chip information
 * 
 * Logs chip model, revision, CPU cores, features, and flash size
 */
void chip_info_log(void);

/**
 * @brief Get chip model name as string
 * 
 * @return Chip model name (e.g., "ESP32-S3", "ESP32-C6")
 */
const char* chip_info_get_model_name(void);

/**
 * @brief Check if running on ESP32-S3
 * 
 * @return true if running on ESP32-S3, false otherwise
 */
bool chip_info_is_esp32s3(void);

/**
 * @brief Check if running on ESP32-C6
 * 
 * @return true if running on ESP32-C6, false otherwise
 */
bool chip_info_is_esp32c6(void);

#ifdef __cplusplus
}
#endif
