/**
 * @file sensor_manager.c
 * @brief Sensor manager implementation for MAX17048 and EZO sensors
 */

#include "sensor_manager.h"
#include "platform/i2c_scanner.h"
#include "drivers/max17048.h"
#include "drivers/ezo_sensor.h"
#include "services/telemetry/mqtt_telemetry.h"
#include "sensors/config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <limits.h>
#include <stddef.h>

static const char *TAG = "SENSOR_MGR";

// Sensor handles
static max17048_t s_battery_monitor;
static bool s_battery_available = false;

#define MAX_EZO_SENSORS 5
static ezo_sensor_t s_ezo_sensors[MAX_EZO_SENSORS];
static uint8_t s_ezo_count = 0;

#define SENSOR_TRIGGER_DELAY_MS 20
#define SENSOR_WAIT_STEP_MS 50
#define SENSOR_MIN_WAIT_MS 750
#define SENSOR_CONSECUTIVE_FAIL_LIMIT 5
#define SENSOR_RECOVERY_RETRY_MS 120000
#define SENSOR_RECOVERY_RETRY_US (SENSOR_RECOVERY_RETRY_MS * 1000ULL)
#define SENSOR_RECOVERY_INIT_DELAY_MS 3000
#define SENSOR_RECOVERY_MAX_ATTEMPTS 5

static uint8_t s_consecutive_failures[MAX_EZO_SENSORS] = {0};
static bool s_sensor_degraded[MAX_EZO_SENSORS] = {0};
static int64_t s_last_recovery_attempt_us[MAX_EZO_SENSORS] = {0};

// EZO sensor type indices
static int s_rtd_index = -1;  // Temperature
static int s_ph_index = -1;   // pH
static int s_ec_index = -1;   // Electrical conductivity
static int s_do_index = -1;   // Dissolved oxygen
static int s_orp_index = -1;  // ORP
static int s_hum_index = -1;  // Humidity

// Cached sensor readings (last successful values) - old per-sensor cache
typedef struct {
    float values[4];
    uint8_t count;
    bool valid;
    uint32_t timestamp_ms;
} cached_sensor_data_t;

static cached_sensor_data_t s_cached_readings[MAX_EZO_SENSORS] = {0};
#define CACHE_TIMEOUT_MS 300000  // 5 minutes - consider cached data stale after this

// Global sensor cache for API access
static sensor_cache_t s_sensor_cache = {0};
static bool s_cache_valid = false;
static SemaphoreHandle_t s_cache_mutex = NULL;
static sensor_cache_listener_t s_cache_listener = NULL;
static void *s_cache_listener_ctx = NULL;

// RTD temperature tracking for compensation
static float s_last_rtd_temp = 25.0f;  // Default fallback temperature
static int64_t s_last_rtd_timestamp_us = 0;  // Timestamp of last RTD reading
#define RTD_TEMP_STALE_THRESHOLD_US (30 * 1000000)  // 30 seconds

// Background reading task
static TaskHandle_t s_reading_task_handle = NULL;
static uint32_t s_reading_interval_sec = 10;
static bool s_reading_paused = false;
static bool s_reading_in_progress = false;
static esp_err_t sensor_manager_refresh_settings_internal(void);

// Forward declaration
static void sensor_reading_task(void *arg);
static uint32_t sensor_manager_get_conversion_delay_ms(const ezo_sensor_t *sensor);
static bool sensor_manager_use_cached_value(uint8_t index, cached_sensor_t *target, uint32_t now_ms);
static void sensor_manager_reset_health_tracking(void);
static void sensor_manager_record_success(uint8_t index, const ezo_sensor_t *sensor);
static void sensor_manager_record_failure(uint8_t index, const ezo_sensor_t *sensor, esp_err_t reason);
static bool sensor_manager_should_attempt_recovery(uint8_t index);
static bool sensor_manager_attempt_recovery(uint8_t index, const ezo_sensor_t *sensor);
static void sensor_manager_publish_health_event(const char *event, const ezo_sensor_t *sensor, const char *detail);

/**
 * @brief Initialize all sensors
 */
esp_err_t sensor_manager_init(void) {
    ESP_LOGI(TAG, "Initializing sensor manager");
    
    i2c_master_bus_handle_t bus_handle = i2c_scanner_get_bus_handle();
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    sensor_manager_reset_health_tracking();
    
    // Initialize MAX17048 battery monitor at 0x36
    if (i2c_scanner_device_exists(0x36)) {
        ESP_LOGI(TAG, "MAX17048 battery monitor detected at 0x36");
        esp_err_t ret = max17048_init(&s_battery_monitor, bus_handle);
        if (ret == ESP_OK) {
            s_battery_available = true;
            ESP_LOGI(TAG, "✓ MAX17048 initialized successfully");
            
            // Read initial values
            float voltage, soc;
            if (max17048_read_voltage(&s_battery_monitor, &voltage) == ESP_OK &&
                max17048_read_soc(&s_battery_monitor, &soc) == ESP_OK) {
                ESP_LOGI(TAG, "  Battery: %.2f V, %.1f%%", voltage, soc);
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize MAX17048");
        }
    }
    
    // Initialize EZO sensors
    // Scan for EZO sensors at known addresses (excluding 0x36 which is MAX17048)
    const sensor_discovery_rules_t *rules = sensor_config_get_discovery_rules();

    for (size_t i = 0; i < rules->ezo_address_count && s_ezo_count < MAX_EZO_SENSORS; i++) {
        uint8_t addr = rules->ezo_addresses[i];

        // Allow bus to settle between sensors per discovery rules
        if (i > 0 && rules->sensor_inter_init_delay_ms > 0) {
            ESP_LOGI(TAG, "Waiting %lu ms before initializing next sensor...",
                     (unsigned long)rules->sensor_inter_init_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(rules->sensor_inter_init_delay_ms));
        }
        
        if (i2c_scanner_device_exists(addr)) {
            ESP_LOGI(TAG, "EZO sensor detected at 0x%02X", addr);
            
            esp_err_t ret = ezo_sensor_init(&s_ezo_sensors[s_ezo_count], bus_handle, addr);
            if (ret == ESP_OK) {
                ezo_sensor_t *sensor = &s_ezo_sensors[s_ezo_count];
                
                ESP_LOGI(TAG, "✓ EZO sensor initialized: Type=%s, Name=%s, FW=%s", 
                         sensor->config.type, sensor->config.name, sensor->config.firmware_version);
                
                // Map sensor type to index
                if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
                    s_rtd_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → Temperature sensor (RTD)");
                } else if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
                    s_ph_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → pH sensor");
                } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
                    s_ec_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → Electrical Conductivity sensor");
                } else if (strcmp(sensor->config.type, EZO_TYPE_DO) == 0) {
                    s_do_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → Dissolved Oxygen sensor");
                } else if (strcmp(sensor->config.type, EZO_TYPE_ORP) == 0) {
                    s_orp_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → ORP sensor");
                } else if (strcmp(sensor->config.type, EZO_TYPE_HUM) == 0) {
                    s_hum_index = s_ezo_count;
                    ESP_LOGI(TAG, "  → Humidity sensor");
                }
                
                s_ezo_count++;
            } else {
                ESP_LOGW(TAG, "Failed to initialize EZO sensor at 0x%02X", addr);
            }
        }
    }
    
    ESP_LOGI(TAG, "Sensor manager initialized: Battery=%s, EZO sensors=%d",
             s_battery_available ? "YES" : "NO", s_ezo_count);

    esp_err_t settings_ret = sensor_manager_refresh_settings_internal();
    if (settings_ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial sensor settings refresh encountered errors: %s", esp_err_to_name(settings_ret));
    }
    
    return ESP_OK;
}

/**
 * @brief Deinitialize all sensors
 */
esp_err_t sensor_manager_deinit(void) {
    // Deinitialize MAX17048
    if (s_battery_available) {
        max17048_deinit(&s_battery_monitor);
        s_battery_available = false;
    }
    
    // Deinitialize EZO sensors
    for (int i = 0; i < s_ezo_count; i++) {
        ezo_sensor_deinit(&s_ezo_sensors[i]);
    }
    s_ezo_count = 0;
    s_rtd_index = -1;
    s_ph_index = -1;
    s_ec_index = -1;
    s_do_index = -1;
    s_orp_index = -1;
    s_hum_index = -1;
    
    // Clear cached readings
    memset(s_cached_readings, 0, sizeof(s_cached_readings));
    sensor_manager_reset_health_tracking();
    
    ESP_LOGI(TAG, "Sensor manager deinitialized");
    return ESP_OK;
}

/**
 * @brief Read battery voltage
 */
esp_err_t sensor_manager_read_battery_voltage(float *voltage) {
    if (!s_battery_available) {
        ESP_LOGW(TAG, "Battery monitor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return max17048_read_voltage(&s_battery_monitor, voltage);
}

/**
 * @brief Read battery percentage
 */
esp_err_t sensor_manager_read_battery_percentage(float *percentage) {
    if (!s_battery_available) {
        ESP_LOGW(TAG, "Battery monitor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return max17048_read_soc(&s_battery_monitor, percentage);
}

/**
 * @brief Read temperature from EZO-RTD
 */
esp_err_t sensor_manager_read_temperature(float *temperature) {
    if (s_rtd_index < 0) {
        ESP_LOGD(TAG, "RTD sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_rtd_index], temperature);
}

/**
 * @brief Read pH from EZO-pH
 */
esp_err_t sensor_manager_read_ph(float *ph) {
    if (s_ph_index < 0) {
        ESP_LOGD(TAG, "pH sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_ph_index], ph);
}

/**
 * @brief Read EC from EZO-EC
 */
esp_err_t sensor_manager_read_ec(float *ec) {
    if (s_ec_index < 0) {
        ESP_LOGD(TAG, "EC sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_ec_index], ec);
}

/**
 * @brief Read DO from EZO-DO
 */
esp_err_t sensor_manager_read_do(float *dox) {
    if (s_do_index < 0) {
        ESP_LOGD(TAG, "DO sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_do_index], dox);
}

/**
 * @brief Read ORP from EZO-ORP
 */
esp_err_t sensor_manager_read_orp(float *orp) {
    if (s_orp_index < 0) {
        ESP_LOGD(TAG, "ORP sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_orp_index], orp);
}

/**
 * @brief Read humidity from EZO-HUM
 */
esp_err_t sensor_manager_read_humidity(float *humidity) {
    if (s_hum_index < 0) {
        ESP_LOGD(TAG, "Humidity sensor not available");
        return ESP_ERR_NOT_FOUND;
    }
    
    return ezo_sensor_read(&s_ezo_sensors[s_hum_index], humidity);
}

/**
 * @brief Get number of EZO sensors
 */
uint8_t sensor_manager_get_ezo_count(void) {
    return s_ezo_count;
}

/**
 * @brief Check if battery monitor is available
 */
bool sensor_manager_has_battery_monitor(void) {
    return s_battery_available;
}

/**
 * @brief Get EZO sensor handle by index
 */
void* sensor_manager_get_ezo_sensor(uint8_t index) {
    if (index >= s_ezo_count) {
        return NULL;
    }
    return &s_ezo_sensors[index];
}

bool sensor_manager_get_ezo_info(uint8_t index, sensor_manager_ezo_info_t *info) {
    if (info == NULL || index >= s_ezo_count) {
        return false;
    }

    const ezo_sensor_t *sensor = &s_ezo_sensors[index];
    strncpy(info->type, sensor->config.type, sizeof(info->type) - 1);
    info->type[sizeof(info->type) - 1] = '\0';
    strncpy(info->name, sensor->config.name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->address = sensor->config.i2c_address;
    return true;
}

/**
 * @brief Read all values from an EZO sensor by index
 * 
 * This function attempts to read fresh sensor data. If the read fails or the sensor
 * is not ready, it returns cached data from the last successful read (if available).
 */
esp_err_t sensor_manager_read_ezo_sensor(uint8_t index, char *sensor_type, float values[4], uint8_t *count) {
    if (index >= s_ezo_count || sensor_type == NULL || values == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ezo_sensor_t *sensor = &s_ezo_sensors[index];
    
    // Debug: Log the sensor type from config
    ESP_LOGD(TAG, "Reading sensor index %d: type='%s' (addr=0x%02X)", 
             index, sensor->config.type, sensor->config.i2c_address);
    
    strncpy(sensor_type, sensor->config.type, 15);
    sensor_type[15] = '\0';
    
    // Try to read fresh data from sensor
    esp_err_t ret = ezo_sensor_read_all(sensor, values, count);
    
    if (ret == ESP_OK) {
        // Success - cache the new readings
        cached_sensor_data_t *cache = &s_cached_readings[index];
        for (uint8_t i = 0; i < *count && i < 4; i++) {
            cache->values[i] = values[i];
        }
        cache->count = *count;
        cache->valid = true;
        cache->timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        return ESP_OK;
    } else {
        // Read failed - try to use cached data
        cached_sensor_data_t *cache = &s_cached_readings[index];
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        if (cache->valid && (now_ms - cache->timestamp_ms) < CACHE_TIMEOUT_MS) {
            // Return cached data
            for (uint8_t i = 0; i < cache->count && i < 4; i++) {
                values[i] = cache->values[i];
            }
            *count = cache->count;
            ESP_LOGD(TAG, "Sensor 0x%02X read failed, using cached data (%lu ms old)", 
                     sensor->config.i2c_address, now_ms - cache->timestamp_ms);
            return ESP_OK;
        }
        
        // No valid cache available
        return ret;
    }
}

/**
 * @brief Rescan I2C bus and reinitialize sensors
 */
esp_err_t sensor_manager_rescan(void) {
    ESP_LOGI(TAG, "Rescanning I2C bus for sensors");
    
    // Deinitialize existing sensors
    sensor_manager_deinit();
    
    // Reinitialize all sensors
    return sensor_manager_init();
}

static esp_err_t sensor_manager_refresh_settings_internal(void) {
    if (s_ezo_count == 0) {
        return ESP_OK;
    }

    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    ESP_LOGI(TAG, "Refreshing settings for %u EZO sensor(s)", s_ezo_count);
    esp_err_t first_err = ESP_OK;

    for (uint8_t i = 0; i < s_ezo_count; i++) {
        ezo_sensor_t *sensor = &s_ezo_sensors[i];
        esp_err_t ret = ezo_sensor_refresh_settings(sensor);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to refresh settings for %s @ 0x%02X: %s",
                     sensor->config.type,
                     sensor->config.i2c_address,
                     esp_err_to_name(ret));
            if (first_err == ESP_OK) {
                first_err = ret;
            }
        }

        if (scheduler_running) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    return first_err;
}

static uint32_t sensor_manager_get_conversion_delay_ms(const ezo_sensor_t *sensor) {
    if (sensor == NULL || sensor->config.type[0] == '\0') {
        return 1000;
    }

    const char *type = sensor->config.type;
    if (strcmp(type, EZO_TYPE_PH) == 0 || strcmp(type, EZO_TYPE_ORP) == 0) {
        return 900;
    }
    if (strcmp(type, EZO_TYPE_EC) == 0) {
        return 1000;
    }
    if (strcmp(type, EZO_TYPE_DO) == 0) {
        return 1300;
    }
    if (strcmp(type, EZO_TYPE_RTD) == 0) {
        return 600;
    }
    if (strcmp(type, EZO_TYPE_HUM) == 0) {
        return 600;
    }

    return 1000;
}

static bool sensor_manager_use_cached_value(uint8_t index, cached_sensor_t *target, uint32_t now_ms) {
    if (index >= MAX_EZO_SENSORS || target == NULL) {
        return false;
    }

    cached_sensor_data_t *cache = &s_cached_readings[index];
    if (!cache->valid) {
        return false;
    }

    uint32_t age_ms = (now_ms >= cache->timestamp_ms)
                        ? (now_ms - cache->timestamp_ms)
                        : (CACHE_TIMEOUT_MS + 1);
    if (cache->timestamp_ms == 0 || age_ms > CACHE_TIMEOUT_MS) {
        return false;
    }

    target->value_count = cache->count;
    if (target->value_count > MAX_SENSOR_VALUES) {
        target->value_count = MAX_SENSOR_VALUES;
    }
    for (uint8_t i = 0; i < target->value_count; i++) {
        target->values[i] = cache->values[i];
    }
    target->valid = true;
    return true;
}

static void sensor_manager_reset_health_tracking(void) {
    memset(s_consecutive_failures, 0, sizeof(s_consecutive_failures));
    memset(s_sensor_degraded, 0, sizeof(s_sensor_degraded));
    memset(s_last_recovery_attempt_us, 0, sizeof(s_last_recovery_attempt_us));
}

static void sensor_manager_publish_health_event(const char *event, const ezo_sensor_t *sensor, const char *detail) {
    if (event == NULL || sensor == NULL) {
        return;
    }

    if (!mqtt_client_is_connected()) {
        return;
    }

    const char *type = (sensor->config.type[0] != '\0') ? sensor->config.type : "UNKNOWN";
    uint8_t address = sensor->config.i2c_address;
    char payload[160];

    if (detail != NULL && detail[0] != '\0') {
        snprintf(payload, sizeof(payload), "%s | %s@0x%02X %s", event, type, address, detail);
    } else {
        snprintf(payload, sizeof(payload), "%s | %s@0x%02X", event, type, address);
    }

    esp_err_t err = mqtt_publish_status(payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish sensor health event '%s': %s", event, esp_err_to_name(err));
    }
}

static void sensor_manager_record_success(uint8_t index, const ezo_sensor_t *sensor) {
    if (index >= MAX_EZO_SENSORS || sensor == NULL) {
        return;
    }

    if (s_consecutive_failures[index] == 0 && !s_sensor_degraded[index]) {
        return;
    }

    s_consecutive_failures[index] = 0;
    if (s_sensor_degraded[index]) {
        s_sensor_degraded[index] = false;
        sensor_manager_publish_health_event("sensor_recovered", sensor, "read_success");
        const char *type = (sensor->config.type[0] != '\0') ? sensor->config.type : "UNKNOWN";
        ESP_LOGI(TAG, "Sensor %s @0x%02X recovered after transient failures",
                 type, sensor->config.i2c_address);
    }
}

static bool sensor_manager_should_attempt_recovery(uint8_t index) {
    if (index >= MAX_EZO_SENSORS) {
        return false;
    }

    int64_t last_attempt = s_last_recovery_attempt_us[index];
    if (last_attempt == 0) {
        return true;
    }

    int64_t elapsed = esp_timer_get_time() - last_attempt;
    return elapsed >= SENSOR_RECOVERY_RETRY_US;
}

static bool sensor_manager_attempt_recovery(uint8_t index, const ezo_sensor_t *sensor_ref) {
    if (index >= s_ezo_count) {
        return false;
    }

    ezo_sensor_t *sensor = &s_ezo_sensors[index];
    const char *type = (sensor->config.type[0] != '\0') ? sensor->config.type : "UNKNOWN";
    uint8_t address = sensor->config.i2c_address;

    i2c_master_bus_handle_t bus_handle = i2c_scanner_get_bus_handle();
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "Cannot attempt recovery for %s @0x%02X: I2C bus unavailable", type, address);
        return false;
    }

    ESP_LOGW(TAG, "Attempting recovery for sensor %s @0x%02X", type, address);
    ezo_sensor_deinit(sensor);

    bool recovered = false;
    for (int attempt = 1; attempt <= SENSOR_RECOVERY_MAX_ATTEMPTS; attempt++) {
        esp_err_t ret = ezo_sensor_init(sensor, bus_handle, address);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Sensor %s @0x%02X recovery succeeded (attempt %d/%d)",
                     type, address, attempt, SENSOR_RECOVERY_MAX_ATTEMPTS);
            s_consecutive_failures[index] = 0;
            s_sensor_degraded[index] = false;
            sensor_manager_publish_health_event("sensor_recovered", sensor, "reinit_success");
            recovered = true;
            break;
        }

        ESP_LOGW(TAG, "Sensor %s @0x%02X recovery attempt %d/%d failed: %s",
                 type, address, attempt, SENSOR_RECOVERY_MAX_ATTEMPTS, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(SENSOR_RECOVERY_INIT_DELAY_MS));
    }

    s_last_recovery_attempt_us[index] = esp_timer_get_time();

    if (!recovered) {
        char detail[80];
        snprintf(detail, sizeof(detail), "type=%s addr=0x%02X cooldown %ums",
             type, address, (unsigned int)SENSOR_RECOVERY_RETRY_MS);
        sensor_manager_publish_health_event("sensor_recovering", sensor_ref != NULL ? sensor_ref : sensor, detail);
        ESP_LOGW(TAG, "Sensor %s @0x%02X recovery deferred for %dms",
                 type, address, (int)SENSOR_RECOVERY_RETRY_MS);
    }

    return recovered;
}

static void sensor_manager_record_failure(uint8_t index, const ezo_sensor_t *sensor, esp_err_t reason) {
    if (index >= MAX_EZO_SENSORS || sensor == NULL) {
        return;
    }

    if (s_consecutive_failures[index] < UINT8_MAX) {
        s_consecutive_failures[index]++;
    }

    const char *type = (sensor->config.type[0] != '\0') ? sensor->config.type : "UNKNOWN";
    uint8_t address = sensor->config.i2c_address;

    ESP_LOGW(TAG, "Sensor %s @0x%02X read failed (%s) [%u/%u]",
             type,
             address,
             esp_err_to_name(reason),
             s_consecutive_failures[index],
             SENSOR_CONSECUTIVE_FAIL_LIMIT);

    if (s_consecutive_failures[index] >= SENSOR_CONSECUTIVE_FAIL_LIMIT) {
        if (!s_sensor_degraded[index]) {
            s_sensor_degraded[index] = true;
            char detail[80];
            snprintf(detail, sizeof(detail), "type=%s addr=0x%02X failures=%u",
                     type, address, s_consecutive_failures[index]);
            sensor_manager_publish_health_event("sensor_degraded", sensor, detail);
            ESP_LOGW(TAG, "Sensor %s @0x%02X marked degraded", type, address);
        }

        if (sensor_manager_should_attempt_recovery(index)) {
            sensor_manager_attempt_recovery(index, sensor);
        }
    }
}

/**
 * @brief Background sensor reading task
 */
static void sensor_reading_task(void *arg) {
    ESP_LOGI(TAG, "Sensor reading task started (interval: %lu seconds)", s_reading_interval_sec);
    
    // CRITICAL: Wait for all sensor initialization to fully complete
    // EZO sensors need time to process initialization commands before being read
    ESP_LOGI(TAG, "Waiting 2 seconds for sensor initialization to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Starting sensor reading loop");
    
    // Perform first read immediately after stabilization
    bool first_read = true;
    
    while (1) {
        // Check if reading is paused
        if (s_reading_paused) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        
        // Wait before next reading (except for first read)
        if (!first_read) {
            vTaskDelay(pdMS_TO_TICKS(s_reading_interval_sec * 1000));
        }
        first_read = false;
        
        // Read all sensors
        if (s_cache_mutex != NULL && xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_reading_in_progress = true;

            sensor_cache_t new_cache;
            memset(&new_cache, 0, sizeof(new_cache));
            new_cache.timestamp_us = esp_timer_get_time();
            
            // Read battery
            if (s_battery_available) {
                float battery_pct;
                if (sensor_manager_read_battery_percentage(&battery_pct) == ESP_OK) {
                    new_cache.battery_percentage = battery_pct;
                    new_cache.battery_valid = true;
                }
            }
            
            // Read RSSI
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                new_cache.rssi = ap_info.rssi;
            }
            
            // Read all EZO sensors and keep cache slots aligned to physical indexes
            const uint8_t cache_capacity = sizeof(s_sensor_cache.sensors) / sizeof(s_sensor_cache.sensors[0]);
            uint8_t total_sensors = s_ezo_count;
            if (total_sensors > cache_capacity) {
                total_sensors = cache_capacity;
            }
            new_cache.sensor_count = total_sensors;

            bool sensor_triggered[MAX_EZO_SENSORS] = {0};
            uint8_t triggered_count = 0;
            uint32_t max_wait_ms = 0;
            
            // Check if we have recent RTD temperature for compensation
            int64_t now_us = esp_timer_get_time();
            bool rtd_temp_valid = (now_us - s_last_rtd_timestamp_us) < RTD_TEMP_STALE_THRESHOLD_US;
            float compensation_temp = rtd_temp_valid ? s_last_rtd_temp : 25.0f;
            
            if (!rtd_temp_valid && s_rtd_index >= 0) {
                ESP_LOGD(TAG, "RTD temperature stale, using fallback 25°C for compensation");
            }

            for (uint8_t i = 0; i < total_sensors; i++) {
                if (s_reading_paused) {
                    ESP_LOGI(TAG, "Sensor reading paused before trigger phase completed (%u/%u)", i, total_sensors);
                    break;
                }

                ezo_sensor_t *sensor = &s_ezo_sensors[i];
                esp_err_t trigger_ret;
                
                // Use temperature-compensated read for pH, EC, and ORP
                const char *type = sensor->config.type;
                bool needs_temp_comp = (strcmp(type, "pH") == 0 || 
                                       strcmp(type, "EC") == 0 || 
                                       strcmp(type, "ORP") == 0);
                
                if (needs_temp_comp && rtd_temp_valid) {
                    trigger_ret = ezo_sensor_start_read_with_temp(sensor, compensation_temp);
                } else {
                    trigger_ret = ezo_sensor_start_read(sensor);
                }
                
                if (trigger_ret == ESP_OK) {
                    sensor_triggered[i] = true;
                    triggered_count++;
                    uint32_t sensor_delay = sensor_manager_get_conversion_delay_ms(sensor);
                    if (sensor_delay > max_wait_ms) {
                        max_wait_ms = sensor_delay;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to trigger sensor %s @0x%02X: %s",
                             sensor->config.type,
                             sensor->config.i2c_address,
                             esp_err_to_name(trigger_ret));
                }

                vTaskDelay(pdMS_TO_TICKS(SENSOR_TRIGGER_DELAY_MS));
            }

            if (s_reading_paused) {
                s_reading_in_progress = false;
                xSemaphoreGive(s_cache_mutex);
                continue;
            }

            if (triggered_count > 0) {
                if (max_wait_ms < SENSOR_MIN_WAIT_MS) {
                    max_wait_ms = SENSOR_MIN_WAIT_MS;
                }

                uint32_t waited_ms = 0;
                while (waited_ms < max_wait_ms && !s_reading_paused) {
                    uint32_t step = SENSOR_WAIT_STEP_MS;
                    if (waited_ms + step > max_wait_ms) {
                        step = max_wait_ms - waited_ms;
                    }
                    vTaskDelay(pdMS_TO_TICKS(step));
                    waited_ms += step;
                }

                if (s_reading_paused) {
                    ESP_LOGI(TAG, "Sensor reading paused while waiting for conversions");
                    s_reading_in_progress = false;
                    xSemaphoreGive(s_cache_mutex);
                    continue;
                }
            }

            uint8_t valid_sensors = 0;
            uint8_t sensors_processed = 0;
            for (uint8_t i = 0; i < total_sensors; i++) {
                cached_sensor_t *cached = &new_cache.sensors[i];
                memset(cached, 0, sizeof(*cached));

                ezo_sensor_t *sensor = &s_ezo_sensors[i];
                
                // Safely copy sensor type with validation
                if (sensor->config.type[0] != '\0' && strnlen(sensor->config.type, sizeof(sensor->config.type)) > 0) {
                    strncpy(cached->sensor_type, sensor->config.type, sizeof(cached->sensor_type) - 1);
                    cached->sensor_type[sizeof(cached->sensor_type) - 1] = '\0';
                    // Verify the copy was successful
                    if (cached->sensor_type[0] == '\0' || strncmp(cached->sensor_type, sensor->config.type, 3) != 0) {
                        ESP_LOGE(TAG, "Sensor type copy failed! Source: '%s', Dest: '%s', Address: 0x%02X", 
                                 sensor->config.type, cached->sensor_type, sensor->config.i2c_address);
                        snprintf(cached->sensor_type, sizeof(cached->sensor_type), "COPY_FAIL_%02X", sensor->config.i2c_address);
                    }
                } else {
                    snprintf(cached->sensor_type, sizeof(cached->sensor_type), "UNKNOWN_%02X", sensor->config.i2c_address);
                    ESP_LOGW(TAG, "Sensor at 0x%02X has invalid type, using fallback name", sensor->config.i2c_address);
                }

                esp_err_t read_ret = ESP_FAIL;
                if (sensor_triggered[i]) {
                    read_ret = ezo_sensor_fetch_all(sensor, cached->values, &cached->value_count);
                    if (read_ret == ESP_ERR_NOT_FINISHED) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        read_ret = ezo_sensor_fetch_all(sensor, cached->values, &cached->value_count);
                    }
                }

                uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (read_ret == ESP_OK) {
                    sensor_manager_record_success(i, sensor);
                    cached->valid = true;
                    cached_sensor_data_t *slot = &s_cached_readings[i];
                    slot->valid = true;
                    slot->count = cached->value_count;
                    slot->timestamp_ms = now_ms;
                    for (uint8_t v = 0; v < cached->value_count && v < MAX_SENSOR_VALUES; v++) {
                        slot->values[v] = cached->values[v];
                    }
                    valid_sensors++;
                    
                    // Update RTD temperature for compensation if this is the RTD sensor
                    if (i == s_rtd_index && cached->value_count > 0) {
                        s_last_rtd_temp = cached->values[0];
                        s_last_rtd_timestamp_us = esp_timer_get_time();
                        ESP_LOGD(TAG, "Updated RTD temperature for compensation: %.2f°C", s_last_rtd_temp);
                    }
                } else {
                    sensor_manager_record_failure(i, sensor, read_ret);
                    if (!sensor_manager_use_cached_value(i, cached, now_ms)) {
                        cached->valid = false;
                    }
                }

                sensors_processed++;

                if (s_reading_paused) {
                    ESP_LOGI(TAG, "Sensor reading paused mid-cycle (%u/%u processed)", sensors_processed, total_sensors);
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(SENSOR_TRIGGER_DELAY_MS));
            }

            // Mark any unused cache slots (including skipped sensors) as invalid to avoid stale data
            for (uint8_t i = sensors_processed; i < cache_capacity; i++) {
                cached_sensor_t *cached = &new_cache.sensors[i];
                memset(cached, 0, sizeof(*cached));
                cached->valid = false;
            }
            
            bool cache_updated = false;
            bool notify_listener = false;
            sensor_cache_t listener_snapshot;
            if (total_sensors == 0) {
                ESP_LOGI(TAG, "Sensor cache refreshed (no sensors detected)");
                memcpy(&s_sensor_cache, &new_cache, sizeof(new_cache));
                cache_updated = true;
            } else if (sensors_processed < total_sensors) {
                ESP_LOGI(TAG, "Sensor cache update interrupted (%u/%u processed, %u valid)",
                         sensors_processed, total_sensors, valid_sensors);
                // Keep previous cache to avoid wiping data used by MQTT/UI
            } else {
                memcpy(&s_sensor_cache, &new_cache, sizeof(new_cache));
                cache_updated = true;
                ESP_LOGI(TAG, "✓ Cache updated (%u/%u sensors valid)",
                         valid_sensors, total_sensors);
            }

            if (cache_updated) {
                s_cache_valid = true;
                if (s_cache_listener != NULL) {
                    listener_snapshot = s_sensor_cache;
                    notify_listener = true;
                }
                
                // Trigger MQTT publish if periodic publishing is disabled (interval=0)
                if (mqtt_get_telemetry_interval() == 0) {
                    ESP_LOGI(TAG, "Triggering MQTT publish (on-read mode)");
                    mqtt_trigger_immediate_publish();
                }
            }

            s_reading_in_progress = false;
            xSemaphoreGive(s_cache_mutex);

            if (notify_listener && s_cache_listener != NULL) {
                s_cache_listener(&listener_snapshot, s_cache_listener_ctx);
            }
        }
    }
}

esp_err_t sensor_manager_start_reading_task(uint32_t interval_sec) {
    if (s_reading_task_handle != NULL) {
        ESP_LOGW(TAG, "Reading task already running");
        return ESP_OK;
    }
    
    // Load saved sensor interval from NVS (or use provided default)
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint32_t saved_interval = interval_sec;
        err = nvs_get_u32(nvs_handle, "sensor_interval", &saved_interval);
        if (err == ESP_OK) {
            interval_sec = saved_interval;
            ESP_LOGI(TAG, "Loaded sensor interval from NVS: %lu seconds", saved_interval);
        }
        nvs_close(nvs_handle);
    }
    
    s_reading_interval_sec = interval_sec;
    
    // Create mutex for cache access
    if (s_cache_mutex == NULL) {
        s_cache_mutex = xSemaphoreCreateMutex();
        if (s_cache_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create cache mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Initialize cache as valid but empty to prevent startup warnings
    s_cache_valid = false;  // Will be set true after first read completes
    
    // Create reading task
#ifdef CONFIG_FREERTOS_UNICORE
    // Single core (ESP32-C6): Run on core 0
    BaseType_t ret = xTaskCreate(
        sensor_reading_task,
        "sensor_read",
        4096,
        NULL,
        5,
        &s_reading_task_handle
    );
#else
    // Dual core (ESP32-S3): Pin to core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        sensor_reading_task,
        "sensor_read",
        4096,
        NULL,
        5,
        &s_reading_task_handle,
        1  // Core 1
    );
#endif
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reading task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Sensor reading task started");
    return ESP_OK;
}

esp_err_t sensor_manager_stop_reading_task(void) {
    if (s_reading_task_handle != NULL) {
        vTaskDelete(s_reading_task_handle);
        s_reading_task_handle = NULL;
        ESP_LOGI(TAG, "Sensor reading task stopped");
    }
    return ESP_OK;
}

esp_err_t sensor_manager_get_cached_data(sensor_cache_t *cache) {
    if (cache == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_cache_valid) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (s_cache_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Copy cached data (thread-safe)
    if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(cache, &s_sensor_cache, sizeof(sensor_cache_t));
        xSemaphoreGive(s_cache_mutex);
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

esp_err_t sensor_manager_set_reading_interval(uint32_t interval_sec) {
    s_reading_interval_sec = interval_sec;
    ESP_LOGI(TAG, "Reading interval updated to %lu seconds", interval_sec);
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, "sensor_interval", interval_sec);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Sensor interval saved to NVS");
        }
        nvs_close(nvs_handle);
    }
    
    return ESP_OK;
}

uint32_t sensor_manager_get_reading_interval(void) {
    return s_reading_interval_sec;
}

esp_err_t sensor_manager_pause_reading(void) {
    s_reading_paused = true;
    ESP_LOGI(TAG, "Sensor reading paused");
    return ESP_OK;
}

esp_err_t sensor_manager_resume_reading(void) {
    s_reading_paused = false;
    ESP_LOGI(TAG, "Sensor reading resumed");
    return ESP_OK;
}

bool sensor_manager_is_reading_in_progress(void) {
    return s_reading_in_progress;
}

bool sensor_manager_is_reading_paused(void) {
    return s_reading_paused;
}

esp_err_t sensor_manager_refresh_settings(void) {
    if (s_ezo_count == 0) {
        return ESP_OK;
    }

    bool resume_required = false;
    bool reading_task_running = (s_reading_task_handle != NULL);

    if (reading_task_running && !sensor_manager_is_reading_paused()) {
        sensor_manager_pause_reading();
        resume_required = true;
    }

    if (reading_task_running) {
        const TickType_t timeout_ticks = pdMS_TO_TICKS(2000);
        TickType_t start = xTaskGetTickCount();
        while (sensor_manager_is_reading_in_progress()) {
            if ((xTaskGetTickCount() - start) > timeout_ticks) {
                ESP_LOGW(TAG, "Timeout waiting for sensor reading task to stop before refreshing settings");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    esp_err_t ret = sensor_manager_refresh_settings_internal();

    if (resume_required) {
        sensor_manager_resume_reading();
    }

    return ret;
}

void sensor_manager_register_cache_listener(sensor_cache_listener_t listener, void *user_ctx) {
    s_cache_listener = listener;
    s_cache_listener_ctx = user_ctx;
}
