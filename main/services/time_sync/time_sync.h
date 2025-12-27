/**
 * @file time_sync.h
 * @brief NTP time synchronization module
 * 
 * Provides automatic time synchronization using SNTP (Simple Network Time Protocol).
 * Synchronizes system time with NTP servers after WiFi connection.
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Time sync status callback
 * 
 * @param synced true if time was successfully synchronized, false on failure
 * @param current_time Pointer to current time structure (valid only if synced=true)
 */
typedef void (*time_sync_callback_t)(bool synced, struct tm *current_time);

/**
 * @brief Initialize NTP time synchronization
 * 
 * Sets up SNTP client with default NTP servers and starts automatic synchronization.
 * Should be called after WiFi connection is established.
 * 
 * @param timezone Timezone string (e.g., "EST5EDT,M3.2.0/2,M11.1.0" for US Eastern)
 *                 Pass NULL to use UTC
 * @param callback Optional callback function called when time is synced (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t time_sync_init(const char *timezone, time_sync_callback_t callback);

/**
 * @brief Check if system time has been synchronized
 * 
 * @return true if time is synced, false otherwise
 */
bool time_sync_is_synced(void);

/**
 * @brief Get current time as string
 * 
 * @param buffer Buffer to store formatted time string
 * @param buffer_size Size of the buffer
 * @param format Time format string (strftime format), NULL for default ISO 8601
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if time not synced yet
 */
esp_err_t time_sync_get_time_string(char *buffer, size_t buffer_size, const char *format);

/**
 * @brief Get current Unix timestamp
 * 
 * @param timestamp Pointer to store the timestamp
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if time not synced yet
 */
esp_err_t time_sync_get_timestamp(time_t *timestamp);

/**
 * @brief Stop time synchronization and cleanup
 */
void time_sync_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_SYNC_H
