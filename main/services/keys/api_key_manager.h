/**
 * @file api_key_manager.h
 * @brief API key management for secure authentication
 *
 * Manages API keys stored in encrypted NVS for:
 * - Local dashboard authentication
 * - Cloud server authentication (sensors.kannacloud.com)
 * - Future API integrations
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum lengths
#define API_KEY_MAX_LENGTH 64
#define API_KEY_NAME_MAX_LENGTH 32
#define API_KEY_MAX_COUNT 10

/**
 * @brief API key types
 */
typedef enum {
    API_KEY_TYPE_LOCAL_DASHBOARD,   // For authenticating to local HTTPS dashboard
    API_KEY_TYPE_CLOUD_SERVER,      // For authenticating to remote cloud server
    API_KEY_TYPE_CUSTOM             // For other integrations
} api_key_type_t;

/**
 * @brief API key structure
 */
typedef struct {
    char name[API_KEY_NAME_MAX_LENGTH];     // Friendly name (e.g., "Dashboard Key", "Cloud Upload Key")
    char key[API_KEY_MAX_LENGTH];           // The actual API key
    api_key_type_t type;                    // Key type
    bool enabled;                           // Whether key is active
    uint32_t created_timestamp;             // Unix timestamp when created
    uint32_t last_used_timestamp;           // Unix timestamp of last use
    uint32_t use_count;                     // Number of times key has been used
} api_key_t;

/**
 * @brief Initialize API key manager
 *
 * Loads API keys from encrypted NVS storage.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t api_key_manager_init(void);

/**
 * @brief Add a new API key
 *
 * @param name Friendly name for the key (e.g., "Dashboard Key")
 * @param key The API key string
 * @param type Type of API key
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max keys reached, error code otherwise
 */
esp_err_t api_key_manager_add(const char *name, const char *key, api_key_type_t type);

/**
 * @brief Delete an API key by name
 *
 * @param name Name of the key to delete
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if key doesn't exist
 */
esp_err_t api_key_manager_delete(const char *name);

/**
 * @brief Enable or disable an API key
 *
 * @param name Name of the key
 * @param enabled true to enable, false to disable
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if key doesn't exist
 */
esp_err_t api_key_manager_set_enabled(const char *name, bool enabled);

/**
 * @brief Validate an API key
 *
 * Checks if the provided key exists, is enabled, and matches a stored key.
 * Updates last_used_timestamp and use_count on successful validation.
 *
 * @param key The API key to validate
 * @param type Optional: validate against specific key type (use -1 to check all types)
 * @return true if key is valid and enabled, false otherwise
 */
bool api_key_manager_validate(const char *key, api_key_type_t type);

/**
 * @brief Get an API key by name
 *
 * @param name Name of the key
 * @param key_out Pointer to store the key information
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if key doesn't exist
 */
esp_err_t api_key_manager_get(const char *name, api_key_t *key_out);

/**
 * @brief Get all API keys
 *
 * @param keys Array to store keys (must be size API_KEY_MAX_COUNT)
 * @param count Pointer to store actual number of keys returned
 * @return ESP_OK on success
 */
esp_err_t api_key_manager_get_all(api_key_t *keys, size_t *count);

/**
 * @brief Get the first API key of a specific type
 *
 * Useful for getting the cloud server API key, dashboard key, etc.
 *
 * @param type Type of key to retrieve
 * @param key_out Pointer to store the key information
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no key of that type exists
 */
esp_err_t api_key_manager_get_by_type(api_key_type_t type, api_key_t *key_out);

/**
 * @brief Generate a random API key
 *
 * Creates a cryptographically secure random key.
 *
 * @param key_out Buffer to store generated key (must be at least API_KEY_MAX_LENGTH bytes)
 * @param length Desired key length (max API_KEY_MAX_LENGTH-1)
 * @return ESP_OK on success
 */
esp_err_t api_key_manager_generate(char *key_out, size_t length);

/**
 * @brief Clear all API keys
 *
 * WARNING: This will delete all stored API keys!
 *
 * @return ESP_OK on success
 */
esp_err_t api_key_manager_clear_all(void);

#ifdef __cplusplus
}
#endif
