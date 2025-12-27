/**
 * @file mqtt_telemetry.h
 * @brief MQTT telemetry client for cloud data streaming
 */

#ifndef MQTT_TELEMETRY_H
#define MQTT_TELEMETRY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT connection state
 */
typedef enum {
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR
} mqtt_state_t;

/**
 * @brief Telemetry data structure (legacy)
 */
typedef struct {
    uint32_t uptime_sec;          // Device uptime in seconds
    uint32_t free_heap;           // Free heap memory in bytes
    int8_t rssi;                  // WiFi signal strength in dBm
    float cpu_temp;               // CPU temperature (if available)
    uint32_t wifi_reconnects;     // Number of WiFi reconnections
    uint32_t mqtt_reconnects;     // Number of MQTT reconnections
} telemetry_data_t;

/**
 * @brief Multi-value sensor reading
 */
#define MAX_SENSOR_VALUES 4
typedef struct {
    char sensor_type[16];  // Sensor type (RTD, pH, EC, HUM, etc.)
    float values[MAX_SENSOR_VALUES];  // Up to 4 values from sensor
    uint8_t value_count;   // Number of valid values
    bool valid;            // Whether this sensor reading is valid
} sensor_reading_t;

/**
 * @brief Sensor data structure for KannaCloud format
 * All sensor fields are optional - set to 0 or NAN if not available
 */
typedef struct {
    float temperature;    // Temperature in Celsius (optional)
    float humidity;       // Relative humidity in % (optional)
    float soil_moisture;  // Soil moisture in % (optional)
    float light_level;    // Light level in lux or raw ADC (optional)
    float ph;             // pH value (optional)
    float ec;             // Electrical conductivity in µS/cm (optional)
    float dissolved_oxygen; // Dissolved oxygen in mg/L (optional)
    float orp;            // Oxidation-reduction potential in mV (optional)
} sensor_data_t;

/**
 * @brief KannaCloud telemetry message structure
 */
typedef struct {
    char device_id[32];          // Device ID (required)
    sensor_data_t sensors;       // Sensor readings (all optional)
    float battery;               // Battery level in % (optional, use NAN if not available)
    int8_t rssi;                 // WiFi signal strength in dBm (optional)
} kannacloud_data_t;

/**
 * @brief Refresh device name from NVS
 * 
 * Called when device name is changed through the dashboard to update the cached value
 * without restarting the device or MQTT connection.
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t mqtt_refresh_device_name(void);

/**
 * @brief Initialize MQTT client
 * 
 * Connects to MQTT broker specified in menuconfig
 * 
 * @param broker_uri MQTT broker URI (e.g., "mqtt://broker.example.com:1883" or "mqtts://broker.example.com:8883")
 * @param username MQTT username (NULL if no authentication required)
 * @param password MQTT password (NULL if no authentication required)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_init(const char *broker_uri, const char *username, const char *password);

/**
 * @brief Start MQTT client and connect to broker
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief Stop MQTT client and disconnect
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_stop(void);



/**
 * @brief Deinitialize MQTT client and free resources
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_deinit(void);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected, false otherwise
 */
bool mqtt_client_is_connected(void);

/**
 * @brief Get current MQTT connection state
 * 
 * @return Current connection state
 */
mqtt_state_t mqtt_client_get_state(void);

/**
 * @brief Publish telemetry data to cloud (legacy)
 * 
 * Publishes to topic: devices/{device_id}/telemetry
 * 
 * @param data Telemetry data structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_telemetry(const telemetry_data_t *data);

/**
 * @brief Publish sensor data to KannaCloud
 * 
 * Publishes to topic: kannacloud/sensor/{device_id}/data
 * JSON format: {"device_id": "...", "sensors": {...}, "battery": ..., "rssi": ...}
 * 
 * @param data KannaCloud telemetry data structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_kannacloud_data(const kannacloud_data_t *data);

/**
 * @brief Publish device status to cloud
 * 
 * Publishes to topic: devices/{device_id}/status
 * 
 * @param status Status message (e.g., "online", "offline", "provisioning")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_status(const char *status);

/**
 * @brief Publish custom JSON message
 * 
 * @param topic MQTT topic
 * @param json_data JSON string to publish
 * @param qos Quality of Service (0, 1, or 2)
 * @param retain Retain flag
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_json(const char *topic, const char *json_data, int qos, bool retain);

/**
 * @brief Subscribe to a topic
 * 
 * @param topic MQTT topic to subscribe
 * @param qos Quality of Service (0, 1, or 2)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from a topic
 * 
 * @param topic MQTT topic to unsubscribe
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_unsubscribe(const char *topic);

/**
 * @brief Set telemetry publishing interval
 * 
 * @param interval_sec Interval in seconds (0 to disable automatic publishing)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_set_telemetry_interval(uint32_t interval_sec);

/**
 * @brief Get current telemetry publishing interval
 * 
 * @return Current interval in seconds (0 means automatic publishing disabled)
 */
uint32_t mqtt_get_telemetry_interval(void);

/**
 * @brief Trigger an immediate MQTT publish
 * 
 * Wakes up the MQTT publish task to send data immediately.
 * Useful when interval is set to 0 (on-read publishing).
 * 
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if task not running
 */
esp_err_t mqtt_trigger_immediate_publish(void);

/**
 * @brief Get device ID for MQTT topics
 * 
 * @param device_id Buffer to store device ID
 * @param size Buffer size
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_get_device_id(char *device_id, size_t size);

/**
 * @brief Get last cached sensor readings (non-blocking)
 * 
 * Returns the most recent sensor readings from the background task.
 * This does NOT perform any I2C operations and is safe to call from HTTP handlers.
 * 
 * @param data Pointer to kannacloud_data_t structure to fill with cached data
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no data available yet
 */
esp_err_t mqtt_get_cached_sensor_data(kannacloud_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // MQTT_TELEMETRY_H
