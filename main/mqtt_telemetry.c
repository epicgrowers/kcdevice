/**
 * @file mqtt_client.c
 * @brief MQTT client implementation for cloud telemetry
 */

#include "mqtt_telemetry.h"
#include "cloud_provisioning.h"
#include "wifi_manager.h"
#include "sensor_manager.h"
#include "ezo_sensor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h" // ESP-IDF MQTT client
#include <string.h>
#include <sys/time.h>
#include <math.h>

static const char *TAG = "MQTT_CLIENT";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static mqtt_state_t s_mqtt_state = MQTT_STATE_DISCONNECTED;
static TaskHandle_t s_publish_task_handle = NULL;
static uint32_t s_publish_interval_sec = 10; // Default: 10 seconds between MQTT publishes
static uint32_t s_mqtt_reconnects = 0;
static char s_device_id[32] = {0};
static char s_device_name[65] = {0};
static char s_mqtt_ca_cert[CLOUD_PROV_MAX_CERT_SIZE] = {0}; // Static buffer for CA certificate

// Forward declarations
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void mqtt_publish_task(void *arg);

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✓ Connected to MQTT broker");
            s_mqtt_state = MQTT_STATE_CONNECTED;
            
            // Subscribe to KannaCloud command topic for this device
            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "kannacloud/sensor/%s/cmd", s_device_id);
            esp_mqtt_client_subscribe(s_mqtt_client, cmd_topic, 1);
            ESP_LOGI(TAG, "Subscribed to: %s", cmd_topic);
            
            // Publish initial connection status
            ESP_LOGI(TAG, "Device ready to publish sensor data");
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            s_mqtt_state = MQTT_STATE_DISCONNECTED;
            s_mqtt_reconnects++;
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Unsubscribed, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Published, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received message on topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
            
            // Parse command (could be extended to handle different commands)
            if (event->data_len > 0) {
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "command");
                    if (cmd && cJSON_IsString(cmd)) {
                        ESP_LOGI(TAG, "Command received: %s", cmd->valuestring);
                        
                        // Handle commands here (reboot, update settings, etc.)
                        if (strcmp(cmd->valuestring, "reboot") == 0) {
                            ESP_LOGW(TAG, "Reboot command received, restarting in 3 seconds...");
                            mqtt_publish_status("rebooting");
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            esp_restart();
                        } else if (strcmp(cmd->valuestring, "ping") == 0) {
                            mqtt_publish_status("pong");
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            s_mqtt_state = MQTT_STATE_ERROR;
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP transport error");
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "Connection refused");
            }
            break;
            
        default:
            break;
    }
}

/**
 * @brief Read sensor values using sensor_manager
 * Non-static to allow access from http_server for dashboard display
 */
float read_temperature(void) {
    float temp = 0.0f;
    // Try to read from EZO-RTD sensor
    if (sensor_manager_read_temperature(&temp) == ESP_OK) {
        return temp;
    }
    // Return 0 if sensor not available (don't send fake data)
    return 0.0f;
}

float read_humidity(void) {
    // EZO-HUM sensor would provide humidity data if connected
    // Return 0 if sensor not available (don't send fake data)
    return 0.0f;
}

float read_soil_moisture(void) {
    float ec = 0.0f;
    // Use EC (electrical conductivity) sensor as soil moisture indicator
    // EC is often used to measure nutrient/moisture content in growing medium
    if (sensor_manager_read_ec(&ec) == ESP_OK) {
        // Convert EC to approximate soil moisture percentage
        // EC typically ranges 0.5-3.0 mS/cm for soil
        // Map this to 0-100% moisture scale (simplified conversion)
        float moisture = (ec / 3000.0f) * 100.0f; // EC in µS/cm
        if (moisture > 100.0f) moisture = 100.0f;
        return moisture;
    }
    // Return 0 if sensor not available (don't send fake data)
    return 0.0f;
}

float read_light_level(void) {
    // No light sensor connected
    // Return 0 if sensor not available (don't send fake data)
    return 0.0f;
}

float read_battery_level(void) {
    float battery = 0.0f;
    // Read from MAX17048 battery monitor
    if (sensor_manager_read_battery_percentage(&battery) == ESP_OK) {
        return battery;
    }
    // Return 0 if sensor not available (don't send fake data)
    return 0.0f;
}

/**
 * @brief MQTT publish task - reads from sensor_manager cache and publishes to MQTT
 */
static void mqtt_publish_task(void *arg)
{
    ESP_LOGI(TAG, "MQTT publish task started (interval: %lu seconds)", s_publish_interval_sec);
    
    while (1) {
        // Wait for next publish cycle FIRST (before checking conditions)
        // If interval is 0, wait for notification (triggered by sensor reads)
        if (s_publish_interval_sec == 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(s_publish_interval_sec * 1000));
        }
        
        // Only publish if connected to MQTT broker
        if (s_mqtt_state != MQTT_STATE_CONNECTED) {
            continue;
        }
        
        // Skip publishing if sensor reading is in progress
        if (sensor_manager_is_reading_in_progress()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Skip publishing if sensor reading is paused (focus mode active)
        if (sensor_manager_is_reading_paused()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Get cached sensor data from sensor_manager (non-blocking)
        sensor_cache_t cache;
        if (sensor_manager_get_cached_data(&cache) != ESP_OK) {
            ESP_LOGW(TAG, "No cached sensor data available yet");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Create JSON from cached data
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        cJSON_AddStringToObject(root, "device_id", s_device_id);
        if (s_device_name[0] != '\0') {
            cJSON_AddStringToObject(root, "device_name", s_device_name);
        }
        
        // Add sensors
        cJSON *sensors = cJSON_CreateObject();
        for (uint8_t i = 0; i < cache.sensor_count && i < 8; i++) {
            cached_sensor_t *sensor = &cache.sensors[i];
            if (!sensor->valid) continue;
            
            if (sensor->value_count == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[0] * 100) / 100);
                cJSON_AddRawToObject(sensors, sensor->sensor_type, buf);
            } else if (sensor->value_count > 1) {
                cJSON *sensor_obj = cJSON_CreateObject();
                if (strcmp(sensor->sensor_type, "HUM") == 0) {
                    char buf[32];
                    if (sensor->value_count >= 1) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[0] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "humidity", buf);
                    }
                    if (sensor->value_count >= 2) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[1] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "air_temp", buf);
                    }
                    if (sensor->value_count >= 3) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[2] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "dew_point", buf);
                    }
                } else if (strcmp(sensor->sensor_type, "EC") == 0) {
                    char buf[32];
                    if (sensor->value_count >= 1) {
                        snprintf(buf, sizeof(buf), "%.0f", floor(sensor->values[0]));
                        cJSON_AddRawToObject(sensor_obj, "conductivity", buf);
                    }
                    if (sensor->value_count >= 2) {
                        snprintf(buf, sizeof(buf), "%.0f", floor(sensor->values[1]));
                        cJSON_AddRawToObject(sensor_obj, "tds", buf);
                    }
                    if (sensor->value_count >= 3) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[2] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "salinity", buf);
                    }
                    if (sensor->value_count >= 4) {
                        snprintf(buf, sizeof(buf), "%.3f", floor(sensor->values[3] * 1000) / 1000);
                        cJSON_AddRawToObject(sensor_obj, "specific_gravity", buf);
                    }
                } else if (strcmp(sensor->sensor_type, "DO") == 0) {
                    char buf[32];
                    if (sensor->value_count >= 1) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[0] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "dissolved_oxygen", buf);
                    }
                    if (sensor->value_count >= 2) {
                        snprintf(buf, sizeof(buf), "%.2f", floor(sensor->values[1] * 100) / 100);
                        cJSON_AddRawToObject(sensor_obj, "saturation", buf);
                    }
                }
                cJSON_AddItemToObject(sensors, sensor->sensor_type, sensor_obj);
            }
        }
        cJSON_AddItemToObject(root, "sensors", sensors);
        
        // Add battery
        if (cache.battery_valid) {
            cJSON_AddNumberToObject(root, "battery", cache.battery_percentage);
        }
        
        // Add RSSI
        cJSON_AddNumberToObject(root, "rssi", cache.rssi);
        
        // Serialize and publish
        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        if (json_str != NULL) {
            ESP_LOGI(TAG, "Publishing JSON: %s", json_str);
            
            char topic[128];
            snprintf(topic, sizeof(topic), "kannacloud/sensor/%s/data", s_device_id);
            
            int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0, 1, 0);
            if (msg_id >= 0) {
                ESP_LOGI(TAG, "✓ MQTT data published successfully");
            }
            free(json_str);
        }
    }
}

esp_err_t mqtt_client_init(const char *broker_uri, const char *username, const char *password)
{
    if (s_mqtt_client != NULL) {
        ESP_LOGW(TAG, "MQTT client already initialized");
        return ESP_OK;
    }
    
    // Load saved MQTT interval from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint32_t saved_interval = 10; // Default
        err = nvs_get_u32(nvs_handle, "mqtt_interval", &saved_interval);
        if (err == ESP_OK) {
            s_publish_interval_sec = saved_interval;
            ESP_LOGI(TAG, "Loaded MQTT interval from NVS: %lu seconds", saved_interval);
        } else {
            ESP_LOGW(TAG, "Failed to load MQTT interval from NVS: %s, using default %lu", esp_err_to_name(err), saved_interval);
            s_publish_interval_sec = saved_interval;
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for MQTT interval: %s, using default 10", esp_err_to_name(err));
        s_publish_interval_sec = 10;
    }
    
    // Get device ID and name from cloud provisioning
    cloud_prov_get_device_id(s_device_id, sizeof(s_device_id));
    cloud_prov_get_device_name(s_device_name, sizeof(s_device_name));
    
    ESP_LOGI(TAG, "Initializing MQTT client");
    ESP_LOGI(TAG, "Broker URI: %s", broker_uri);
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);
    if (s_device_name[0] != '\0') {
        ESP_LOGI(TAG, "Device Name: %s", s_device_name);
    }
    if (username) {
        ESP_LOGI(TAG, "Username: %s", username);
    }
    
    // Check if this is a secure connection (mqtts://)
    bool is_secure = (strncmp(broker_uri, "mqtts://", 8) == 0);
    
    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = s_device_id,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };
    
    // Add TLS configuration for secure connections
    if (is_secure) {
        ESP_LOGI(TAG, "Configuring MQTTS with TLS encryption");
        
        // Load CA certificate from NVS into static buffer
        size_t ca_cert_len = 0;
        esp_err_t ret = cloud_prov_get_mqtt_ca_cert(s_mqtt_ca_cert, &ca_cert_len);
        
        if (ret == ESP_OK && ca_cert_len > 0) {
            ESP_LOGI(TAG, "Loaded CA certificate for MQTTS (%zu bytes)", ca_cert_len);
            
            // Log first and last parts of certificate for verification
            char preview_start[150];
            char preview_end[150];
            size_t preview_len = ca_cert_len < 149 ? ca_cert_len : 149;
            memcpy(preview_start, s_mqtt_ca_cert, preview_len);
            preview_start[preview_len] = '\0';
            
            if (ca_cert_len > 149) {
                size_t end_offset = ca_cert_len - 100;
                memcpy(preview_end, s_mqtt_ca_cert + end_offset, 100);
                preview_end[100] = '\0';
            }
            
            ESP_LOGI(TAG, "Certificate START: %s", preview_start);
            if (ca_cert_len > 149) {
                ESP_LOGI(TAG, "Certificate END: %s", preview_end);
            }
            ESP_LOGI(TAG, "Certificate null-terminated: %s", (s_mqtt_ca_cert[ca_cert_len] == '\0') ? "YES" : "NO");
            
            // The certificate must be null-terminated for mbedTLS
            s_mqtt_ca_cert[ca_cert_len] = '\0';
            
            mqtt_cfg.broker.verification.certificate = s_mqtt_ca_cert;
            mqtt_cfg.broker.verification.certificate_len = ca_cert_len + 1; // Include null terminator
            ESP_LOGI(TAG, "✓ TLS encryption enabled with CA certificate verification");
        } else {
            ESP_LOGW(TAG, "CA certificate not found (%s), skipping verification", esp_err_to_name(ret));
            mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        }
    }
    
    // Create MQTT client
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    // Register event handler
    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "✓ MQTT client initialized successfully");
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting MQTT client...");
    s_mqtt_state = MQTT_STATE_CONNECTING;
    
    esp_err_t ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        s_mqtt_state = MQTT_STATE_ERROR;
        return ret;
    }
    
    // Create MQTT publish task
    if (s_publish_task_handle == NULL) {
#ifdef CONFIG_FREERTOS_UNICORE
        // Single core (ESP32-C6): Run on core 0
        BaseType_t task_ret = xTaskCreate(
            mqtt_publish_task,
            "mqtt_publish",
            4096,
            NULL,
            5,
            &s_publish_task_handle
        );
#else
        // Dual core (ESP32-S3): Pin to core 0 (network core)
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            mqtt_publish_task,
            "mqtt_publish",
            4096,
            NULL,
            5,
            &s_publish_task_handle,
            0  // Pin to Core 0 (network core)
        );
#endif
        
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create MQTT publish task");
            return ESP_FAIL;
        }
        
        ESP_LOGI(TAG, "✓ MQTT publish task started (publish interval: %lu seconds)", s_publish_interval_sec);
    }
    
    return ESP_OK;
}

esp_err_t mqtt_refresh_device_name(void)
{
    // Fetch the updated device name from NVS
    char new_device_name[65];
    esp_err_t err = cloud_prov_get_device_name(new_device_name, sizeof(new_device_name));
    
    if (err == ESP_OK) {
        // Update the cached device name
        strncpy(s_device_name, new_device_name, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
        
        if (s_device_name[0] != '\0') {
            ESP_LOGI(TAG, "Device name updated: %s", s_device_name);
        } else {
            ESP_LOGI(TAG, "Device name cleared");
        }
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to refresh device name: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
}

esp_err_t mqtt_client_stop(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_OK;
    }
    
    // Stop MQTT publish task
    if (s_publish_task_handle != NULL) {
        vTaskDelete(s_publish_task_handle);
        s_publish_task_handle = NULL;
    }
    
    // Publish offline status before disconnecting
    if (s_mqtt_state == MQTT_STATE_CONNECTED) {
        mqtt_publish_status("offline");
        vTaskDelay(pdMS_TO_TICKS(500)); // Give time for message to send
    }
    
    ESP_LOGI(TAG, "Stopping MQTT client");
    esp_err_t ret = esp_mqtt_client_stop(s_mqtt_client);
    s_mqtt_state = MQTT_STATE_DISCONNECTED;
    
    return ret;
}

esp_err_t mqtt_client_deinit(void)
{
    mqtt_client_stop();
    
    if (s_mqtt_client != NULL) {
        ESP_LOGI(TAG, "Deinitializing MQTT client");
        esp_err_t ret = esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_mqtt_state = MQTT_STATE_DISCONNECTED;
        return ret;
    }
    
    return ESP_OK;
}

bool mqtt_client_is_connected(void)
{
    return s_mqtt_state == MQTT_STATE_CONNECTED;
}

mqtt_state_t mqtt_get_state(void)
{
    return s_mqtt_state;
}



esp_err_t mqtt_publish_telemetry(const telemetry_data_t *data)
{
    if (s_mqtt_client == NULL || s_mqtt_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create JSON payload
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(root, "uptime", data->uptime_sec);
    cJSON_AddNumberToObject(root, "free_heap", data->free_heap);
    cJSON_AddNumberToObject(root, "rssi", data->rssi);
    cJSON_AddNumberToObject(root, "cpu_temp", data->cpu_temp);
    cJSON_AddNumberToObject(root, "wifi_reconnects", data->wifi_reconnects);
    cJSON_AddNumberToObject(root, "mqtt_reconnects", data->mqtt_reconnects);
    
    // Add timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(root, "timestamp", tv.tv_sec);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Publish to telemetry topic
    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/telemetry", s_device_id);
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0, 1, 0);
    free(json_str);
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish telemetry");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Telemetry published (msg_id: %d)", msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_kannacloud_data(const kannacloud_data_t *data)
{
    if (s_mqtt_client == NULL || s_mqtt_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create JSON payload following KannaCloud format
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Add device_id (required)
    cJSON_AddStringToObject(root, "device_id", data->device_id);
    
    // Add device_name if available
    if (s_device_name[0] != '\0') {
        cJSON_AddStringToObject(root, "device_name", s_device_name);
    }
    
    // Add sensors object - read all EZO sensors dynamically
    cJSON *sensors = cJSON_CreateObject();
    if (sensors == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    uint8_t sensor_count = sensor_manager_get_ezo_count();
    for (uint8_t i = 0; i < sensor_count; i++) {
        char sensor_type[16];
        float values[4];
        uint8_t value_count;
        
        if (sensor_manager_read_ezo_sensor(i, sensor_type, values, &value_count) == ESP_OK) {
            if (value_count == 1) {
                // Single value sensor
                cJSON_AddNumberToObject(sensors, sensor_type, values[0]);
            } else if (value_count > 1) {
                // Multi-value sensor - create object with named fields
                cJSON *sensor_obj = cJSON_CreateObject();
                
                // Define field names based on sensor type
                if (strcmp(sensor_type, "HUM") == 0) {
                    // Get HUM sensor config to determine which parameters are enabled
                    ezo_sensor_t *sensor = (ezo_sensor_t *)sensor_manager_get_ezo_sensor(i);
                    if (sensor != NULL && sensor->config.hum.param_count > 0) {
                        // Use dynamic parameter mapping based on enabled outputs
                        for (uint8_t j = 0; j < value_count && j < sensor->config.hum.param_count; j++) {
                            const char *param = sensor->config.hum.param_order[j];
                            const char *field_name = NULL;
                            
                            if (strcmp(param, "HUM") == 0) {
                                field_name = "humidity";
                            } else if (strcmp(param, "T") == 0) {
                                field_name = "air_temp";
                            } else if (strcmp(param, "DEW") == 0) {
                                field_name = "dew_point";
                            }
                            
                            if (field_name != NULL) {
                                cJSON_AddNumberToObject(sensor_obj, field_name, values[j]);
                            }
                        }
                    } else {
                        // Fallback if config unavailable
                        if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "humidity", values[0]);
                        if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "air_temp", values[1]);
                        if (value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "dew_point", values[2]);
                    }
                } else if (strcmp(sensor_type, "ORP") == 0) {
                    if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "orp", values[0]);
                } else if (strcmp(sensor_type, "DO") == 0) {
                    if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "dissolved_oxygen", values[0]);
                    if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "saturation", values[1]);
                } else if (strcmp(sensor_type, "EC") == 0) {
                    if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "conductivity", values[0]);
                    if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "tds", values[1]);
                    if (value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "salinity", values[2]);
                    if (value_count >= 4) cJSON_AddNumberToObject(sensor_obj, "specific_gravity", values[3]);
                } else {
                    // Unknown multi-value sensor - use generic field names
                    for (uint8_t j = 0; j < value_count; j++) {
                        char field_name[16];
                        snprintf(field_name, sizeof(field_name), "value_%d", j);
                        cJSON_AddNumberToObject(sensor_obj, field_name, values[j]);
                    }
                }
                
                cJSON_AddItemToObject(sensors, sensor_type, sensor_obj);
            }
        }
    }
    
    cJSON_AddItemToObject(root, "sensors", sensors);
    
    // Add battery level (optional)
    if (!isnan(data->battery)) {
        cJSON_AddNumberToObject(root, "battery", data->battery);
    }
    
    // Add RSSI
    cJSON_AddNumberToObject(root, "rssi", data->rssi);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Debug: Log the exact JSON being published
    ESP_LOGI(TAG, "Publishing JSON: %s", json_str);
    
    // Publish to KannaCloud topic: kannacloud/sensor/{device_id}/data
    char topic[128];
    snprintf(topic, sizeof(topic), "kannacloud/sensor/%s/data", data->device_id);
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0, 1, 0); // QoS 1
    free(json_str);
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish KannaCloud data");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "KannaCloud data published to %s (msg_id: %d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char *status)
{
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create JSON payload
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(root, "status", status);
    
    // Add timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(root, "timestamp", tv.tv_sec);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Publish to status topic
    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/status", s_device_id);
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0, 1, 1); // QoS 1, Retain
    free(json_str);
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Status published: %s (msg_id: %d)", status, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_json(const char *topic, const char *json_data, int qos, bool retain)
{
    if (s_mqtt_client == NULL || s_mqtt_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (topic == NULL || json_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_data, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Published to %s (msg_id: %d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_subscribe(const char *topic, int qos)
{
    if (s_mqtt_client == NULL || s_mqtt_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Subscribed to %s (msg_id: %d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_unsubscribe(const char *topic)
{
    if (s_mqtt_client == NULL || s_mqtt_state != MQTT_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int msg_id = esp_mqtt_client_unsubscribe(s_mqtt_client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to unsubscribe from %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Unsubscribed from %s (msg_id: %d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_set_telemetry_interval(uint32_t interval_sec)
{
    uint32_t old_interval = s_publish_interval_sec;
    s_publish_interval_sec = interval_sec;
    
    if (interval_sec == 0) {
        ESP_LOGI(TAG, "MQTT periodic publishing disabled (will publish on sensor read only)");
    } else {
        ESP_LOGI(TAG, "MQTT publish interval updated to %lu seconds", interval_sec);
        
        // If changing from 0 to non-zero, wake up the task so it starts periodic publishing
        if (old_interval == 0 && s_publish_task_handle != NULL) {
            ESP_LOGI(TAG, "Waking up MQTT task to start periodic publishing");
            xTaskNotifyGive(s_publish_task_handle);
        }
    }
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, "mqtt_interval", interval_sec);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "MQTT interval %lu saved to NVS", interval_sec);
            } else {
                ESP_LOGE(TAG, "Failed to commit MQTT interval to NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Failed to set MQTT interval in NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for MQTT interval: %s", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

uint32_t mqtt_get_telemetry_interval(void)
{
    return s_publish_interval_sec;
}

esp_err_t mqtt_trigger_immediate_publish(void)
{
    if (s_publish_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Wake up the publish task
    xTaskNotifyGive(s_publish_task_handle);
    return ESP_OK;
}

esp_err_t mqtt_get_device_id(char *device_id, size_t size)
{
    if (device_id == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(device_id, s_device_id, size - 1);
    device_id[size - 1] = '\0';
    
    return ESP_OK;
}


