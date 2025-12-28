#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize WiFi manager
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi network with given credentials
 * 
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char* ssid, const char* password);

/**
 * @brief Check if WiFi is connected
 * 
 * @return true if connected, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get stored WiFi credentials from NVS
 * 
 * @param ssid Buffer to store SSID (min 33 bytes)
 * @param password Buffer to store password (min 64 bytes)
 * @return ESP_OK if credentials found, ESP_ERR_NVS_NOT_FOUND if not
 */
esp_err_t wifi_manager_get_stored_credentials(char* ssid, char* password);

/**
 * @brief Disconnect from WiFi
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Clear stored WiFi credentials
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_clear_credentials(void);

/**
 * @brief Save WiFi credentials to NVS (without connecting)
 * 
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_save_credentials(const char* ssid, const char* password);

#endif // WIFI_MANAGER_H
