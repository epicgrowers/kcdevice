#ifndef SECURITY_H
#define SECURITY_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize security features including NVS encryption
 * 
 * This function:
 * - Initializes NVS encryption with HMAC-based key protection
 * - Generates encryption keys on first boot (stored in eFuse)
 * - Enables hardware-based key protection
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t security_init(void);

/**
 * @brief Check if NVS encryption is enabled and working
 * 
 * @return true if encryption is active, false otherwise
 */
bool security_is_nvs_encrypted(void);

/**
 * @brief Check if flash encryption is enabled
 * 
 * @return true if flash encryption is enabled, false otherwise
 */
bool security_is_flash_encrypted(void);

/**
 * @brief Get security status information
 * 
 * @param info Buffer to store status string
 * @param len Length of buffer
 */
void security_get_status(char* info, size_t len);

#endif // SECURITY_H
