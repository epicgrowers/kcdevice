/**
 * @file cloud_provisioning.h
 * @brief Cloud provisioning and certificate management
 * 
 * Handles automatic device provisioning with ssl.kannacloud.com:
 * - Generates device-specific SSL certificates
 * - Downloads and stores certificates
 * - Registers device with cloud server
 */

#ifndef CLOUD_PROVISIONING_H
#define CLOUD_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration
#define CLOUD_PROV_SSL_MANAGER_URL "https://ssl.kannacloud.com"

// Device certificate info
#define CLOUD_PROV_MAX_CERT_SIZE (4096)
#define CLOUD_PROV_MAX_KEY_SIZE (4096)

/**
 * @brief Provisioning status callback
 * 
 * @param success true if provisioning succeeded, false otherwise
 * @param message Status message or error description
 */
typedef void (*cloud_prov_callback_t)(bool success, const char *message);

/**
 * @brief Initialize cloud provisioning module
 * 
 * Must be called after WiFi connection and time sync.
 * 
 * @param callback Optional callback for provisioning status updates
 * @return ESP_OK on success
 */
esp_err_t cloud_prov_init(cloud_prov_callback_t callback);

/**
 * @brief Start automatic device provisioning
 * 
 * This will:
 * 1. Check if device already has certificates
 * 2. If not, request new certificates from ssl.kannacloud.com
 * 3. Download private key and certificate
 * 4. Store them in NVS for future use
 * 5. Trigger callback with status
 * 
 * @return ESP_OK if provisioning started, error code otherwise
 */
esp_err_t cloud_prov_provision_device(void);

/**
 * @brief Check if device has valid certificates
 * 
 * @return true if certificates exist and are valid
 */
bool cloud_prov_has_certificates(void);

/**
 * @brief Get device certificate
 * 
 * @param cert_out Buffer to store certificate (must be CLOUD_PROV_MAX_CERT_SIZE bytes)
 * @param cert_len Pointer to store actual certificate length
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no certificate
 */
esp_err_t cloud_prov_get_certificate(char *cert_out, size_t *cert_len);

/**
 * @brief Get device private key
 * 
 * @param key_out Buffer to store private key (must be CLOUD_PROV_MAX_KEY_SIZE bytes)
 * @param key_len Pointer to store actual key length
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no key
 */
esp_err_t cloud_prov_get_private_key(char *key_out, size_t *key_len);

/**
 * @brief Get device unique ID (immutable MAC-based)
 * 
 * Returns the unique device ID based on MAC address (kc-XXXXXXXXXXXX).
 * This ID is immutable and cannot be changed by the user.
 * 
 * @param id_out Buffer to store device ID
 * @param id_len Size of buffer (minimum 20 bytes)
 * @return ESP_OK on success
 */
esp_err_t cloud_prov_get_device_id(char *id_out, size_t id_len);

/**
 * @brief Get user-friendly device name
 * 
 * Returns the custom device name if set, otherwise returns empty string.
 * 
 * @param name_out Buffer to store device name
 * @param name_len Size of buffer (minimum 65 bytes)
 * @return ESP_OK on success
 */
esp_err_t cloud_prov_get_device_name(char *name_out, size_t name_len);

/**
 * @brief Set user-friendly device name
 * 
 * Stores a custom device name for human-readable identification.
 * Name must be 1-64 characters.
 * 
 * @param device_name Custom device name string
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if validation fails
 */
esp_err_t cloud_prov_set_device_name(const char *device_name);

/**
 * @brief Clear device name
 * 
 * Removes the custom device name.
 * 
 * @return ESP_OK on success
 */
esp_err_t cloud_prov_clear_device_name(void);

/**
 * @brief Clear stored certificates (force re-provisioning)
 * 
 * @return ESP_OK on success
 */
esp_err_t cloud_prov_clear_certificates(void);

/**
 * @brief Download MQTT CA certificate from server
 * 
 * Downloads the CA certificate needed for MQTTS connection from
 * https://sensors.kannacloud.com/static/ca.crt and stores it in NVS.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cloud_prov_download_mqtt_ca_cert(void);

/**
 * @brief Get MQTT CA certificate
 * 
 * @param cert_out Buffer to store CA certificate (must be CLOUD_PROV_MAX_CERT_SIZE bytes)
 * @param cert_len Pointer to store actual certificate length
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no certificate
 */
esp_err_t cloud_prov_get_mqtt_ca_cert(char *cert_out, size_t *cert_len);

#ifdef __cplusplus
}
#endif

#endif // CLOUD_PROVISIONING_H
