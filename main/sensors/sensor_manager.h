/**
 * @file sensor_manager.h
 * @brief Sensor manager for handling MAX17048 and EZO sensors
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all sensors
 * 
 * Scans I2C bus and initializes MAX17048 battery monitor and EZO water sensors
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_init(void);

/**
 * @brief Deinitialize all sensors
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_deinit(void);

/**
 * @brief Read battery voltage from MAX17048
 * 
 * @param voltage Pointer to store voltage value
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_battery_voltage(float *voltage);

/**
 * @brief Read battery percentage from MAX17048
 * 
 * @param percentage Pointer to store battery percentage (0-100%)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_battery_percentage(float *percentage);

/**
 * @brief Read temperature from EZO-RTD sensor
 * 
 * @param temperature Pointer to store temperature value
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_temperature(float *temperature);

/**
 * @brief Read pH from EZO-pH sensor
 * 
 * @param ph Pointer to store pH value
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_ph(float *ph);

/**
 * @brief Read electrical conductivity from EZO-EC sensor
 * 
 * @param ec Pointer to store EC value (µS/cm)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_ec(float *ec);

/**
 * @brief Read dissolved oxygen from EZO-DO sensor
 * 
 * @param dox Pointer to store DO value (mg/L)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_do(float *dox);

/**
 * @brief Read ORP (oxidation-reduction potential) from EZO-ORP sensor
 * 
 * @param orp Pointer to store ORP value (mV)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_orp(float *orp);

/**
 * @brief Read humidity from EZO-HUM sensor
 * 
 * @param humidity Pointer to store humidity value (%)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_humidity(float *humidity);

/**
 * @brief Get number of detected EZO sensors
 * 
 * @return uint8_t Number of EZO sensors found
 */
uint8_t sensor_manager_get_ezo_count(void);

/**
 * @brief Check if battery monitor is available
 * 
 * @return true if MAX17048 is present
 */
bool sensor_manager_has_battery_monitor(void);

/**
 * @brief Get EZO sensor handle by index
 * 
 * @param index Sensor index (0 to sensor_manager_get_ezo_count()-1)
 * @return Pointer to EZO sensor handle, NULL if invalid index
 */
void* sensor_manager_get_ezo_sensor(uint8_t index);

typedef struct {
    char type[16];
    char name[32];
    uint8_t address;
} sensor_manager_ezo_info_t;

bool sensor_manager_get_ezo_info(uint8_t index, sensor_manager_ezo_info_t *info);

/**
 * @brief Read all values from an EZO sensor by index
 * 
 * @param index Sensor index
 * @param sensor_type Buffer to store sensor type (min 16 bytes)
 * @param values Array to store readings (up to 4 values)
 * @param count Pointer to store number of values read
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_read_ezo_sensor(uint8_t index, char *sensor_type, float values[4], uint8_t *count);

/**
 * @brief Rescan I2C bus and reinitialize all sensors
 * 
 * Useful after hot-swapping sensors (with power cycle)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_rescan(void);

/**
 * @brief Cached sensor data structure
 */
#define MAX_SENSOR_VALUES 4
typedef struct {
    char sensor_type[16];
    float values[MAX_SENSOR_VALUES];
    uint8_t value_count;
    bool valid;
} cached_sensor_t;

typedef struct {
    cached_sensor_t sensors[8];  // Support up to 8 EZO sensors
    uint8_t sensor_count;
    float battery_percentage;
    bool battery_valid;
    int8_t rssi;
    uint64_t timestamp_us;
} sensor_cache_t;

typedef void (*sensor_cache_listener_t)(const sensor_cache_t *cache, void *user_ctx);

void sensor_manager_register_cache_listener(sensor_cache_listener_t listener, void *user_ctx);

/**
 * @brief Start background sensor reading task
 * 
 * Starts a task that periodically reads all sensors and updates the cache.
 * 
 * @param interval_sec Reading interval in seconds (default 10)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_start_reading_task(uint32_t interval_sec);

/**
 * @brief Stop background sensor reading task
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_stop_reading_task(void);

/**
 * @brief Get cached sensor data (non-blocking, no I2C operations)
 * 
 * Returns the most recent sensor readings from the background task.
 * This is safe to call from any context including HTTP handlers.
 * 
 * @param cache Pointer to sensor_cache_t structure to fill
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND if no data yet
 */
esp_err_t sensor_manager_get_cached_data(sensor_cache_t *cache);

/**
 * @brief Set sensor reading interval
 * 
 * @param interval_sec New interval in seconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_set_reading_interval(uint32_t interval_sec);

/**
 * @brief Get current sensor reading interval
 * 
 * @return Current interval in seconds
 */
uint32_t sensor_manager_get_reading_interval(void);

/**
 * @brief Pause sensor reading task
 * 
 * Useful for performing manual I2C operations from web interface
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_pause_reading(void);

/**
 * @brief Resume sensor reading task
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_manager_resume_reading(void);

/**
 * @brief Check if sensor reading is currently in progress
 * 
 * @return true if currently reading sensors, false otherwise
 */
bool sensor_manager_is_reading_in_progress(void);

/**
 * @brief Check if the sensor reading task is currently paused
 *
 * @return true if paused, false otherwise
 */
bool sensor_manager_is_reading_paused(void);

/**
 * @brief Refresh cached sensor settings (calibration status, mode, compensation)
 *
 * Safely pauses the background reading task if necessary while issuing the
 * required I2C commands, then resumes it when finished (unless it was already
 * paused by the caller).
 */
esp_err_t sensor_manager_refresh_settings(void);

#ifdef __cplusplus
}
#endif
