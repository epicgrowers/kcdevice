#include "http_handlers_sensors.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "services/core/services.h"
#include "services/core/services_provisioning_bridge.h"
#include "services/telemetry/mqtt_telemetry.h"

#define SENSOR_CALIBRATE_URI   "/api/sensors/calibrate/"
#define SENSOR_COMP_URI        "/api/sensors/compensate/"
#define SENSOR_MODE_URI        "/api/sensors/mode/"
#define SENSOR_POWER_URI       "/api/sensors/power/"
#define SENSOR_STATUS_URI      "/api/sensors/status/"
#define SENSOR_SAMPLE_URI      "/api/sensors/sample/"

static const char *TAG = "SERVICES:HTTP:SENSORS";
static SemaphoreHandle_t s_sensor_guard_mutex;

void sensor_read_guard_acquire(sensor_read_guard_t *guard)
{
    if (guard == NULL) {
        return;
    }

    guard->mutex_acquired = false;
    if (s_sensor_guard_mutex == NULL) {
        s_sensor_guard_mutex = xSemaphoreCreateMutex();
        if (s_sensor_guard_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create sensor guard mutex");
        }
    }

    if (s_sensor_guard_mutex != NULL) {
        if (xSemaphoreTake(s_sensor_guard_mutex, portMAX_DELAY) == pdTRUE) {
            guard->mutex_acquired = true;
        } else {
            ESP_LOGW(TAG, "Failed to acquire sensor guard mutex");
        }
    }

    guard->should_resume = false;

    bool already_paused = sensor_manager_is_reading_paused();
    if (!already_paused) {
        if (sensor_manager_pause_reading() == ESP_OK) {
            guard->should_resume = true;
        } else {
            ESP_LOGW(TAG, "Failed to pause sensor reading task before action");
        }
    }

    const TickType_t timeout_ticks = pdMS_TO_TICKS(EZO_LONG_WAIT_MS + 2000);
    TickType_t start = xTaskGetTickCount();
    while (sensor_manager_is_reading_in_progress()) {
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ESP_LOGW(TAG, "Timeout waiting for sensor reading task to idle");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void sensor_read_guard_release(sensor_read_guard_t *guard)
{
    if (guard == NULL) {
        return;
    }

    if (guard->should_resume) {
        sensor_manager_resume_reading();
    }
    guard->should_resume = false;

    if (guard->mutex_acquired && s_sensor_guard_mutex != NULL) {
        xSemaphoreGive(s_sensor_guard_mutex);
        guard->mutex_acquired = false;
    }
}

bool parse_sensor_address_from_uri(const char *uri, const char *prefix, uint8_t *address)
{
    if (uri == NULL || prefix == NULL || address == NULL) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        return false;
    }
    const char *addr_str = uri + prefix_len;
    if (*addr_str == '\0') {
        return false;
    }
    char *endptr = NULL;
    long value = strtol(addr_str, &endptr, 0);
    if (endptr == NULL || *endptr != '\0' || value < 0 || value > 255) {
        return false;
    }
    *address = (uint8_t)value;
    return true;
}

ezo_sensor_t *find_sensor_by_address(uint8_t address)
{
    uint8_t ezo_count = sensor_manager_get_ezo_count();
    for (uint8_t i = 0; i < ezo_count; i++) {
        ezo_sensor_t *sensor = (ezo_sensor_t *)sensor_manager_get_ezo_sensor(i);
        if (sensor != NULL && sensor->config.i2c_address == address) {
            return sensor;
        }
    }
    return NULL;
}

void add_capabilities_array(cJSON *parent, uint32_t flags)
{
    cJSON *caps = cJSON_CreateArray();
    if (caps == NULL) {
        return;
    }
    if (flags & EZO_CAP_CALIBRATION) cJSON_AddItemToArray(caps, cJSON_CreateString("calibration"));
    if (flags & EZO_CAP_TEMP_COMP)   cJSON_AddItemToArray(caps, cJSON_CreateString("temp_comp"));
    if (flags & EZO_CAP_MODE)        cJSON_AddItemToArray(caps, cJSON_CreateString("mode"));
    if (flags & EZO_CAP_SLEEP)       cJSON_AddItemToArray(caps, cJSON_CreateString("sleep"));
    if (flags & EZO_CAP_OFFSET)      cJSON_AddItemToArray(caps, cJSON_CreateString("offset"));
    cJSON_AddItemToObject(parent, "capabilities", caps);
}

void append_sensor_runtime_info(cJSON *json, ezo_sensor_t *sensor)
{
    if (json == NULL || sensor == NULL) {
        return;
    }

    if (sensor->config.capability_flags & EZO_CAP_CALIBRATION) {
        if (sensor->config.calibration_status_valid && sensor->config.calibration_status[0] != '\0') {
            cJSON_AddStringToObject(json, "calibration_status", sensor->config.calibration_status);
        } else {
            cJSON_AddStringToObject(json, "calibration_status", "unknown");
        }
    }

    if ((sensor->config.capability_flags & EZO_CAP_TEMP_COMP) && strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        if (sensor->config.temp_comp_valid) {
            cJSON_AddNumberToObject(json, "temperature_comp", sensor->config.temp_compensation);
        }
    }

    if (sensor->config.capability_flags & EZO_CAP_MODE) {
        cJSON_AddBoolToObject(json, "continuous_mode", sensor->config.continuous_mode);
    }

    if (sensor->config.capability_flags & EZO_CAP_SLEEP) {
        cJSON_AddBoolToObject(json, "sleeping", sensor->config.sleeping);
    }
}

cJSON *build_sensor_json(ezo_sensor_t *sensor, int index, bool include_runtime)
{
    if (sensor == NULL) {
        return NULL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }

    if (index >= 0) {
        cJSON_AddNumberToObject(obj, "index", index);
    }
    cJSON_AddNumberToObject(obj, "address", sensor->config.i2c_address);
    cJSON_AddStringToObject(obj, "type", sensor->config.type);
    cJSON_AddStringToObject(obj, "name", sensor->config.name);
    cJSON_AddStringToObject(obj, "firmware", sensor->config.firmware_version);
    cJSON_AddBoolToObject(obj, "led", sensor->config.led_control);
    cJSON_AddBoolToObject(obj, "plock", sensor->config.protocol_lock);
    add_capabilities_array(obj, sensor->config.capability_flags);

    if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
        cJSON_AddStringToObject(obj, "scale", (const char[]){sensor->config.rtd.temperature_scale, '\0'});
    } else if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        cJSON_AddBoolToObject(obj, "extended_scale", sensor->config.ph.extended_scale);
    } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
        cJSON_AddNumberToObject(obj, "probe_type", sensor->config.ec.probe_type);
        cJSON_AddNumberToObject(obj, "tds_factor", sensor->config.ec.tds_conversion_factor);
        cJSON_AddBoolToObject(obj, "param_ec", sensor->config.ec.param_ec);
        cJSON_AddBoolToObject(obj, "param_tds", sensor->config.ec.param_tds);
        cJSON_AddBoolToObject(obj, "param_s", sensor->config.ec.param_s);
        cJSON_AddBoolToObject(obj, "param_sg", sensor->config.ec.param_sg);
    } else if (strcmp(sensor->config.type, EZO_TYPE_HUM) == 0) {
        cJSON_AddBoolToObject(obj, "param_hum", sensor->config.hum.param_hum);
        cJSON_AddBoolToObject(obj, "param_t", sensor->config.hum.param_t);
        cJSON_AddBoolToObject(obj, "param_dew", sensor->config.hum.param_dew);
    }

    if (include_runtime) {
        append_sensor_runtime_info(obj, sensor);
    }

    return obj;
}

cJSON *parse_request_json_body(httpd_req_t *req, char **raw_buffer)
{
    if (req->content_len <= 0) {
        return NULL;
    }

    char *buffer = malloc(req->content_len + 1);
    if (buffer == NULL) {
        return NULL;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buffer + total, req->content_len - total);
        if (received <= 0) {
            free(buffer);
            return NULL;
        }
        total += received;
    }
    buffer[total] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        free(buffer);
        if (raw_buffer != NULL) {
            *raw_buffer = NULL;
        }
        return NULL;
    }

    if (raw_buffer != NULL) {
        *raw_buffer = buffer;
    } else {
        free(buffer);
    }
    return json;
}

void add_sample_readings_to_json(cJSON *json, const char *type, const float values[], uint8_t count)
{
    if (json == NULL || type == NULL || values == NULL || count == 0) {
        return;
    }

    bool is_multi_value = false;
    if (strcmp(type, EZO_TYPE_EC) == 0 || strcmp(type, EZO_TYPE_HUM) == 0 || strcmp(type, EZO_TYPE_DO) == 0) {
        is_multi_value = true;
    }

    if (!is_multi_value && count == 1) {
        cJSON_AddNumberToObject(json, "reading", values[0]);
        return;
    }

    cJSON *reading = cJSON_CreateObject();
    if (reading == NULL) {
        return;
    }

    if (strcmp(type, EZO_TYPE_EC) == 0) {
        if (count >= 1) cJSON_AddNumberToObject(reading, "conductivity", values[0]);
        if (count >= 2) cJSON_AddNumberToObject(reading, "tds", values[1]);
        if (count >= 3) cJSON_AddNumberToObject(reading, "salinity", values[2]);
        if (count >= 4) cJSON_AddNumberToObject(reading, "specific_gravity", values[3]);
    } else if (strcmp(type, EZO_TYPE_HUM) == 0) {
        if (count >= 1) cJSON_AddNumberToObject(reading, "humidity", values[0]);
        if (count >= 2) cJSON_AddNumberToObject(reading, "air_temp", values[1]);
        if (count >= 3) cJSON_AddNumberToObject(reading, "dew_point", values[2]);
    } else if (strcmp(type, EZO_TYPE_DO) == 0) {
        if (count >= 1) cJSON_AddNumberToObject(reading, "dissolved_oxygen", values[0]);
        if (count >= 2) cJSON_AddNumberToObject(reading, "saturation", values[1]);
    } else {
        for (uint8_t i = 0; i < count; i++) {
            char key[12];
            snprintf(key, sizeof(key), "value%d", i + 1);
            cJSON_AddNumberToObject(reading, key, values[i]);
        }
    }

    cJSON_AddItemToObject(json, "reading", reading);
}

esp_err_t send_sensor_success_response(httpd_req_t *req, ezo_sensor_t *sensor)
{
    if (sensor != NULL) {
        esp_err_t refresh = ezo_sensor_refresh_settings(sensor);
        if (refresh != ESP_OK) {
            ESP_LOGW(TAG, "Failed to refresh sensor settings before response: %s", esp_err_to_name(refresh));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "status", "success");
    cJSON *sensor_json = build_sensor_json(sensor, -1, true);
    if (sensor_json != NULL) {
        cJSON_AddItemToObject(root, "sensor", sensor_json);
    }

    const char *response = cJSON_PrintUnformatted(root);
    if (response == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(root);
    return ESP_OK;
}


static esp_err_t api_sensors_pause_handler(httpd_req_t *req)
{
    sensor_manager_pause_reading();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"paused\"}");
    return ESP_OK;
}

/**
 * @brief POST /api/sensors/resume - Resume sensor reading task
 */
static esp_err_t api_sensors_resume_handler(httpd_req_t *req)
{
    sensor_manager_resume_reading();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"resumed\"}");
    return ESP_OK;
}

/**
 * @brief GET /api/device/info - Get device ID and name
 */
static esp_err_t api_device_info_get_handler(httpd_req_t *req)
{
    char device_id[SERVICES_DEVICE_ID_MAX_LEN] = {0};
    char device_name[SERVICES_DEVICE_NAME_MAX_LEN] = {0};
    
    esp_err_t err = services_provisioning_load_device_id(device_id, sizeof(device_id));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get device ID");
        return ESP_FAIL;
    }
    
    if (services_provisioning_load_device_name(device_name, sizeof(device_name)) != ESP_OK) {
        device_name[0] = '\0';
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "device_name", device_name);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    
    return ESP_OK;
}

/**
 * @brief POST /api/device/name - Set device name
 */
static esp_err_t api_device_name_set_handler(httpd_req_t *req)
{
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *device_name_json = cJSON_GetObjectItem(root, "device_name");
    if (device_name_json == NULL || !cJSON_IsString(device_name_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_name field");
        return ESP_FAIL;
    }
    
    const char *device_name = device_name_json->valuestring;
    
    // Empty string means clear device name
    esp_err_t err;
    if (device_name[0] == '\0') {
        err = services_provisioning_clear_device_name();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Device name cleared");
        }
    } else {
        err = services_provisioning_set_device_name(device_name);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Device name set: %s", device_name);
        }
    }
    
    cJSON_Delete(root);
    
    // Notify MQTT module to refresh device name
    if (err == ESP_OK) {
        mqtt_refresh_device_name();
    }
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                "Invalid device name: must be 1-64 characters");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save device name");
        }
        return ESP_FAIL;
    }
    
    // Return the new device name
    char new_device_name[65];
    if (services_provisioning_load_device_name(new_device_name, sizeof(new_device_name)) != ESP_OK) {
        new_device_name[0] = '\0';
    }
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "saved");
    cJSON_AddStringToObject(response, "device_name", new_device_name);
    
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    
    return ESP_OK;
}

/**
 * @brief GET /api/sensors - Get list of all sensors with their configurations
 * Returns cached configuration loaded at boot - no I2C polling
 */
static esp_err_t api_sensors_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *sensors = cJSON_CreateArray();
    
    // Add battery monitor if available
    if (sensor_manager_has_battery_monitor()) {
        cJSON *battery = cJSON_CreateObject();
        cJSON_AddStringToObject(battery, "type", "MAX17048");
        cJSON_AddNumberToObject(battery, "address", 0x36);
        cJSON_AddStringToObject(battery, "name", "Battery Monitor");
        cJSON_AddStringToObject(battery, "description", "Li+ Battery Fuel Gauge");
        cJSON_AddItemToArray(sensors, battery);
    }
    
    // Add EZO sensors
    uint8_t ezo_count = sensor_manager_get_ezo_count();
    for (uint8_t i = 0; i < ezo_count; i++) {
        ezo_sensor_t *sensor = (ezo_sensor_t*)sensor_manager_get_ezo_sensor(i);
        if (sensor != NULL) {
            cJSON *ezo = build_sensor_json(sensor, i, true);
            if (ezo != NULL) {
                cJSON_AddItemToArray(sensors, ezo);
            }
        }
    }
    
    cJSON_AddItemToObject(root, "sensors", sensors);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(sensors));
    
    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    
    free((void*)response);
    cJSON_Delete(root);
    
    return ESP_OK;
}

/**
 * @brief POST /api/sensors/rescan - Rescan I2C bus for sensors
 */
static esp_err_t api_sensors_rescan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Rescanning I2C bus for sensors");
    
    esp_err_t ret = sensor_manager_rescan();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", ret == ESP_OK ? "success" : "error");
    cJSON_AddNumberToObject(root, "battery", sensor_manager_has_battery_monitor() ? 1 : 0);
    cJSON_AddNumberToObject(root, "ezo_count", sensor_manager_get_ezo_count());
    
    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    
    free((void*)response);
    cJSON_Delete(root);
    
    return ESP_OK;
}

/**
 * @brief POST /api/sensors/config - Update sensor configuration
 * Body: {"address": 99, "led": 1, "name": "MySensor", "scale": "F", etc}
 */
static esp_err_t api_sensors_config_handler(httpd_req_t *req)
{
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *address_json = cJSON_GetObjectItem(root, "address");
    if (address_json == NULL || !cJSON_IsNumber(address_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing address");
        return ESP_FAIL;
    }
    
    uint8_t address = (uint8_t)address_json->valueint;
    
    // Find sensor by address
    ezo_sensor_t *sensor = NULL;
    uint8_t ezo_count = sensor_manager_get_ezo_count();
    for (uint8_t i = 0; i < ezo_count; i++) {
        ezo_sensor_t *s = (ezo_sensor_t*)sensor_manager_get_ezo_sensor(i);
        if (s != NULL && s->config.i2c_address == address) {
            sensor = s;
            break;
        }
    }
    
    if (sensor == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }
    
    // Update LED
    cJSON *led = cJSON_GetObjectItem(root, "led");
    if (led != NULL && cJSON_IsBool(led)) {
        ezo_sensor_set_led(sensor, cJSON_IsTrue(led));
    }
    
    // Update name with validation
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name != NULL && cJSON_IsString(name)) {
        const char *name_str = name->valuestring;
        size_t name_len = strlen(name_str);
        
        // Validate name: 1-16 characters, alphanumeric and underscore only
        if (name_len == 0 || name_len > 16) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name must be 1-16 characters");
            return ESP_FAIL;
        }
        
        // Check for valid characters (alphanumeric and underscore only)
        bool valid = true;
        for (size_t i = 0; i < name_len; i++) {
            char c = name_str[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                  (c >= '0' && c <= '9') || c == '_')) {
                valid = false;
                break;
            }
        }
        
        if (!valid) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name must contain only letters, numbers, and underscores");
            return ESP_FAIL;
        }
        
        esp_err_t name_ret = ezo_sensor_set_name(sensor, name_str);
        if (name_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set sensor name: %s", esp_err_to_name(name_ret));
        }
    }
    
    // Update protocol lock
    cJSON *plock = cJSON_GetObjectItem(root, "plock");
    if (plock != NULL && cJSON_IsBool(plock)) {
        ezo_sensor_set_plock(sensor, cJSON_IsTrue(plock));
    }
    
    // Type-specific updates
    if (strcmp(sensor->config.type, "RTD") == 0) {
        cJSON *scale = cJSON_GetObjectItem(root, "scale");
        if (scale != NULL && cJSON_IsString(scale) && strlen(scale->valuestring) > 0) {
            ezo_rtd_set_scale(sensor, scale->valuestring[0]);
        }
    } else if (strcmp(sensor->config.type, "pH") == 0) {
        cJSON *ext_scale = cJSON_GetObjectItem(root, "extended_scale");
        if (ext_scale != NULL && cJSON_IsBool(ext_scale)) {
            ezo_ph_set_extended_scale(sensor, cJSON_IsTrue(ext_scale));
        }
    } else if (strcmp(sensor->config.type, "EC") == 0) {
        cJSON *probe = cJSON_GetObjectItem(root, "probe_type");
        if (probe != NULL && cJSON_IsNumber(probe)) {
            ezo_ec_set_probe_type(sensor, (float)probe->valuedouble);
        }
        
        cJSON *tds = cJSON_GetObjectItem(root, "tds_factor");
        if (tds != NULL && cJSON_IsNumber(tds)) {
            ezo_ec_set_tds_factor(sensor, (float)tds->valuedouble);
        }
    }
    
    cJSON_Delete(root);
    
    // Refresh ALL sensor settings after update to sync cache with hardware
    esp_err_t refresh_ret = sensor_manager_refresh_settings();
    if (refresh_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh sensor settings: %s", esp_err_to_name(refresh_ret));
    }
    
    return send_sensor_success_response(req, sensor);
}

/**
 * @brief POST /api/sensors/config/batch - Update multiple sensor configurations
 * Body: {"sensors": [{"address": 99, "led": true, "name": "pH1"}, {"address": 100, "led": false}]}
 */
static esp_err_t api_sensors_config_batch_handler(httpd_req_t *req)
{
    char content[2048];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *sensors_array = cJSON_GetObjectItem(root, "sensors");
    if (sensors_array == NULL || !cJSON_IsArray(sensors_array)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sensors array");
        return ESP_FAIL;
    }
    
    int array_size = cJSON_GetArraySize(sensors_array);
    if (array_size == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty sensors array");
        return ESP_FAIL;
    }
    
    if (array_size > 10) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Max 10 sensors per batch");
        return ESP_FAIL;
    }
    
    // Pause sensor readings during batch update
    sensor_manager_pause_reading();
    
    // Wait 500ms to ensure sensor task has fully stopped
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Build response
    cJSON *response_root = cJSON_CreateObject();
    cJSON *results = cJSON_CreateArray();
    int updated_count = 0;
    int failed_count = 0;
    
    // Process each sensor configuration
    for (int i = 0; i < array_size; i++) {
        cJSON *sensor_config = cJSON_GetArrayItem(sensors_array, i);
        if (!cJSON_IsObject(sensor_config)) {
            failed_count++;
            cJSON *error_result = cJSON_CreateObject();
            cJSON_AddNumberToObject(error_result, "index", i);
            cJSON_AddStringToObject(error_result, "status", "error");
            cJSON_AddStringToObject(error_result, "error", "Invalid sensor config object");
            cJSON_AddItemToArray(results, error_result);
            continue;
        }
        
        cJSON *address_json = cJSON_GetObjectItem(sensor_config, "address");
        if (address_json == NULL || !cJSON_IsNumber(address_json)) {
            failed_count++;
            cJSON *error_result = cJSON_CreateObject();
            cJSON_AddNumberToObject(error_result, "index", i);
            cJSON_AddStringToObject(error_result, "status", "error");
            cJSON_AddStringToObject(error_result, "error", "Missing address");
            cJSON_AddItemToArray(results, error_result);
            continue;
        }
        
        uint8_t address = (uint8_t)address_json->valueint;
        
        // Find sensor by address
        ezo_sensor_t *sensor = NULL;
        uint8_t ezo_count = sensor_manager_get_ezo_count();
        for (uint8_t j = 0; j < ezo_count; j++) {
            ezo_sensor_t *s = (ezo_sensor_t*)sensor_manager_get_ezo_sensor(j);
            if (s != NULL && s->config.i2c_address == address) {
                sensor = s;
                break;
            }
        }
        
        if (sensor == NULL) {
            failed_count++;
            cJSON *error_result = cJSON_CreateObject();
            cJSON_AddNumberToObject(error_result, "address", address);
            cJSON_AddStringToObject(error_result, "status", "error");
            cJSON_AddStringToObject(error_result, "error", "Sensor not found");
            cJSON_AddItemToArray(results, error_result);
            continue;
        }
        
        bool has_error = false;
        char error_msg[128] = {0};
        
        // Update LED
        cJSON *led = cJSON_GetObjectItem(sensor_config, "led");
        if (led != NULL && cJSON_IsBool(led)) {
            ezo_sensor_set_led(sensor, cJSON_IsTrue(led));
        }
        
        // Update name with validation
        cJSON *name = cJSON_GetObjectItem(sensor_config, "name");
        if (name != NULL && cJSON_IsString(name)) {
            const char *name_str = name->valuestring;
            size_t name_len = strlen(name_str);
            
            if (name_len == 0 || name_len > 16) {
                has_error = true;
                snprintf(error_msg, sizeof(error_msg), "Name must be 1-16 characters");
            } else {
                // Check for valid characters
                bool valid = true;
                for (size_t k = 0; k < name_len; k++) {
                    char c = name_str[k];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                          (c >= '0' && c <= '9') || c == '_')) {
                        valid = false;
                        break;
                    }
                }
                
                if (!valid) {
                    has_error = true;
                    snprintf(error_msg, sizeof(error_msg), "Name must contain only letters, numbers, and underscores");
                } else {
                    esp_err_t name_ret = ezo_sensor_set_name(sensor, name_str);
                    if (name_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to set sensor name for address %d: %s", address, esp_err_to_name(name_ret));
                    }
                }
            }
        }
        
        // Update protocol lock
        cJSON *plock = cJSON_GetObjectItem(sensor_config, "plock");
        if (plock != NULL && cJSON_IsBool(plock)) {
            ezo_sensor_set_plock(sensor, cJSON_IsTrue(plock));
        }
        
        // Type-specific updates
        if (strcmp(sensor->config.type, "RTD") == 0) {
            cJSON *scale = cJSON_GetObjectItem(sensor_config, "scale");
            if (scale != NULL && cJSON_IsString(scale) && strlen(scale->valuestring) > 0) {
                ezo_rtd_set_scale(sensor, scale->valuestring[0]);
            }
        } else if (strcmp(sensor->config.type, "pH") == 0) {
            cJSON *ext_scale = cJSON_GetObjectItem(sensor_config, "extended_scale");
            if (ext_scale != NULL && cJSON_IsBool(ext_scale)) {
                ezo_ph_set_extended_scale(sensor, cJSON_IsTrue(ext_scale));
            }
        } else if (strcmp(sensor->config.type, "EC") == 0) {
            cJSON *probe = cJSON_GetObjectItem(sensor_config, "probe_type");
            if (probe != NULL && cJSON_IsNumber(probe)) {
                ezo_ec_set_probe_type(sensor, (float)probe->valuedouble);
            }
            
            cJSON *tds = cJSON_GetObjectItem(sensor_config, "tds_factor");
            if (tds != NULL && cJSON_IsNumber(tds)) {
                ezo_ec_set_tds_factor(sensor, (float)tds->valuedouble);
            }
        }
        
        // Note: Skip refresh_settings here to avoid pausing sensors repeatedly
        // Settings are already applied via the set functions above
        // The normal sensor reading cycle will query updated values
        
        // Add result
        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "address", address);
        
        if (has_error) {
            failed_count++;
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error", error_msg);
        } else {
            updated_count++;
            cJSON_AddStringToObject(result, "status", "success");
            cJSON *sensor_json = build_sensor_json(sensor, -1, false);
            if (sensor_json != NULL) {
                cJSON_AddItemToObject(result, "sensor", sensor_json);
            }
        }
        
        cJSON_AddItemToArray(results, result);
    }
    
    // Build final response
    if (failed_count == 0) {
        cJSON_AddStringToObject(response_root, "status", "success");
    } else if (updated_count == 0) {
        cJSON_AddStringToObject(response_root, "status", "failed");
    } else {
        cJSON_AddStringToObject(response_root, "status", "partial");
    }
    
    cJSON_AddNumberToObject(response_root, "updated", updated_count);
    cJSON_AddNumberToObject(response_root, "failed", failed_count);
    cJSON_AddItemToObject(response_root, "results", results);
    
    const char *response_str = cJSON_PrintUnformatted(response_root);
    if (response_str == NULL) {
        cJSON_Delete(response_root);
        cJSON_Delete(root);
        sensor_manager_resume_reading();  // Resume before error
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_str);
    free((void*)response_str);
    cJSON_Delete(response_root);
    cJSON_Delete(root);
    
    // Refresh ALL sensor settings after batch update to sync cache with hardware
    esp_err_t refresh_ret = sensor_manager_refresh_settings();
    if (refresh_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh sensor settings after batch update: %s", esp_err_to_name(refresh_ret));
    }
    
    // Resume sensor readings after batch update
    sensor_manager_resume_reading();
    
    return ESP_OK;
}

static esp_err_t api_sensor_calibrate_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_CALIBRATE_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    sensor_read_guard_t guard;
    bool guard_active = false;

    if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        cJSON *point = cJSON_GetObjectItem(payload, "point");
        if (point == NULL || !cJSON_IsString(point)) {
            cJSON_Delete(payload);
            free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing calibration point");
            return ESP_FAIL;
        }
        const char *point_str = point->valuestring;
        float default_value = 0.0f;
        if (strcmp(point_str, "mid") == 0) default_value = 7.00f;
        else if (strcmp(point_str, "low") == 0) default_value = 4.00f;
        else if (strcmp(point_str, "high") == 0) default_value = 10.00f;

        cJSON *value = cJSON_GetObjectItem(payload, "value");
        float cal_value = default_value;
        if (value != NULL && cJSON_IsNumber(value)) {
            cal_value = (float)value->valuedouble;
        }
        sensor_read_guard_acquire(&guard);
        guard_active = true;
        ret = ezo_ph_calibrate(sensor, point_str, cal_value);
    } else if (strcmp(sensor->config.type, EZO_TYPE_ORP) == 0) {
        bool clear = false;
        cJSON *point = cJSON_GetObjectItem(payload, "point");
        if (point != NULL && cJSON_IsString(point) && strcmp(point->valuestring, "clear") == 0) {
            clear = true;
        }
        cJSON *clear_flag = cJSON_GetObjectItem(payload, "clear");
        if (clear_flag != NULL && cJSON_IsBool(clear_flag)) {
            clear = cJSON_IsTrue(clear_flag);
        }

        sensor_read_guard_acquire(&guard);
        guard_active = true;

        if (clear) {
            ret = ezo_orp_calibrate(sensor, -1000.0f);
        } else {
            cJSON *value = cJSON_GetObjectItem(payload, "value");
            if (value == NULL || !cJSON_IsNumber(value)) {
                cJSON_Delete(payload);
                free(raw);
                sensor_read_guard_release(&guard);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing calibration value");
                return ESP_FAIL;
            }
            ret = ezo_orp_calibrate(sensor, (float)value->valuedouble);
        }
    } else if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
        bool clear = false;
        cJSON *point = cJSON_GetObjectItem(payload, "point");
        if (point != NULL && cJSON_IsString(point) && strcmp(point->valuestring, "clear") == 0) {
            clear = true;
        }

        float reference_temp = 0.0f;
        if (!clear) {
            cJSON *temperature = cJSON_GetObjectItem(payload, "temperature");
            if (temperature == NULL || !cJSON_IsNumber(temperature)) {
                cJSON_Delete(payload);
                free(raw);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing temperature value");
                return ESP_FAIL;
            }
            reference_temp = (float)temperature->valuedouble;
        }

        sensor_read_guard_acquire(&guard);
        guard_active = true;
        ret = ezo_rtd_calibrate(sensor, clear ? -1000.0f : reference_temp);
    } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
        cJSON *point = cJSON_GetObjectItem(payload, "point");
        if (point == NULL || !cJSON_IsString(point)) {
            cJSON_Delete(payload);
            free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing calibration point");
            return ESP_FAIL;
        }

        const char *point_str = point->valuestring;
        bool point_needs_value = (strcmp(point_str, "low") == 0) || (strcmp(point_str, "high") == 0);
        uint32_t cal_value = 0;
        if (point_needs_value) {
            cJSON *value = cJSON_GetObjectItem(payload, "value");
            if (value == NULL || !cJSON_IsNumber(value) || value->valuedouble <= 0) {
                cJSON_Delete(payload);
                free(raw);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid calibration value");
                return ESP_FAIL;
            }
            cal_value = (uint32_t)(value->valuedouble + 0.5);
        }

        sensor_read_guard_acquire(&guard);
        guard_active = true;
        ret = ezo_ec_calibrate(sensor, point_str, cal_value);
    } else if (strcmp(sensor->config.type, EZO_TYPE_DO) == 0) {
        cJSON *point = cJSON_GetObjectItem(payload, "point");
        if (point == NULL || !cJSON_IsString(point)) {
            cJSON_Delete(payload);
            free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing calibration point");
            return ESP_FAIL;
        }

        sensor_read_guard_acquire(&guard);
        guard_active = true;
        ret = ezo_do_calibrate(sensor, point->valuestring);
    }

    cJSON_Delete(payload);
    free(raw);

    if (ret != ESP_OK) {
        if (guard_active) {
            sensor_read_guard_release(&guard);
        }
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Calibration not supported for this sensor");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Calibration failed");
        }
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    if (guard_active) {
        sensor_read_guard_release(&guard);
    }
    return resp;
}

static esp_err_t api_sensor_compensation_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_COMP_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    if (strcmp(sensor->config.type, EZO_TYPE_PH) != 0) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Temperature compensation not supported");
        return ESP_FAIL;
    }

    cJSON *temp = cJSON_GetObjectItem(payload, "temp_c");
    if (temp == NULL || !cJSON_IsNumber(temp)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing temp_c value");
        return ESP_FAIL;
    }
    float target = (float)temp->valuedouble;
    cJSON_Delete(payload);
    free(raw);

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_ph_set_temperature_comp(sensor, target);
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set temperature compensation");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_mode_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_MODE_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (!(sensor->config.capability_flags & EZO_CAP_MODE)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sensor does not support continuous mode");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *continuous = cJSON_GetObjectItem(payload, "continuous");
    if (continuous == NULL || !cJSON_IsBool(continuous)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing continuous flag");
        return ESP_FAIL;
    }

    bool enable = cJSON_IsTrue(continuous);
    cJSON_Delete(payload);
    free(raw);

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_sensor_set_continuous_mode(sensor, enable);
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update mode");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_power_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_POWER_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (!(sensor->config.capability_flags & EZO_CAP_SLEEP)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sensor does not support sleep");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *sleep_flag = cJSON_GetObjectItem(payload, "sleep");
    if (sleep_flag == NULL || !cJSON_IsBool(sleep_flag)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sleep flag");
        return ESP_FAIL;
    }

    bool sleep = cJSON_IsTrue(sleep_flag);
    cJSON_Delete(payload);
    free(raw);

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = sleep ? ezo_sensor_sleep(sensor) : ezo_sensor_wake(sensor);
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to change power state");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_status_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_STATUS_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t refresh = ezo_sensor_refresh_settings(sensor);
    if (refresh != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh sensor %02X before status read: %s", address, esp_err_to_name(refresh));
    }

    cJSON *sensor_json = build_sensor_json(sensor, -1, true);
    if (sensor_json == NULL) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize sensor");
        return ESP_FAIL;
    }

    const char *payload = cJSON_PrintUnformatted(sensor_json);
    cJSON_Delete(sensor_json);
    if (payload == NULL) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize sensor");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload);
    free((void*)payload);
    sensor_read_guard_release(&guard);
    return ESP_OK;
}

static esp_err_t api_sensor_sample_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, SENSOR_SAMPLE_URI, &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    float values[4] = {0};
    uint8_t count = 0;

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);
    esp_err_t ret = ezo_sensor_read_all(sensor, values, &count);
    sensor_read_guard_release(&guard);

    if (ret != ESP_OK || count == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read sensor");
        return ESP_FAIL;
    }

    cJSON *sensor_json = build_sensor_json(sensor, -1, true);
    if (sensor_json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize sensor");
        return ESP_FAIL;
    }

    uint64_t timestamp_ms = esp_timer_get_time() / 1000ULL;
    cJSON_AddNumberToObject(sensor_json, "timestamp_ms", (double)timestamp_ms);
    cJSON_AddNumberToObject(sensor_json, "value_count", count);

    cJSON *raw_array = cJSON_CreateArray();
    if (raw_array != NULL) {
        for (uint8_t i = 0; i < count; i++) {
            cJSON_AddItemToArray(raw_array, cJSON_CreateNumber(values[i]));
        }
        cJSON_AddItemToObject(sensor_json, "raw", raw_array);
    }

    add_sample_readings_to_json(sensor_json, sensor->config.type, values, count);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(sensor_json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate response");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddItemToObject(root, "sensor", sensor_json);

    const char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode response");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload);
    free((void*)payload);
    cJSON_Delete(root);
    return ESP_OK;
}

// New advanced feature handlers
static esp_err_t api_sensor_export_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/export/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    char export_data[128] = {0};
    esp_err_t ret = ezo_sensor_export_calibration(sensor, export_data, sizeof(export_data));
    
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Export failed");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "calibration_data", export_data);
    cJSON_AddNumberToObject(response, "address", address);

    const char *payload = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload);
    free((void*)payload);
    sensor_read_guard_release(&guard);
    return ESP_OK;
}

static esp_err_t api_sensor_import_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/import/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cal_data = cJSON_GetObjectItem(payload, "calibration_data");
    if (cal_data == NULL || !cJSON_IsString(cal_data)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing calibration_data");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_sensor_import_calibration(sensor, cal_data->valuestring);
    cJSON_Delete(payload);
    free(raw);

    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Import failed");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_find_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/find/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_sensor_find(sensor);
    
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Find command failed");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "blinking");
    cJSON_AddNumberToObject(response, "address", address);

    const char *payload_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload_str);
    free((void*)payload_str);
    sensor_read_guard_release(&guard);
    return ESP_OK;
}

static esp_err_t api_sensor_device_status_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/device-status/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    char status_data[128] = {0};
    esp_err_t ret = ezo_sensor_get_status(sensor, status_data, sizeof(status_data));
    
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status query failed");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "device_status", status_data);
    cJSON_AddNumberToObject(response, "address", address);

    const char *payload = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload);
    free((void*)payload);
    sensor_read_guard_release(&guard);
    return ESP_OK;
}

static esp_err_t api_sensor_slope_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/slope/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (strcmp(sensor->config.type, EZO_TYPE_PH) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Slope only available for pH sensors");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    char slope_data[64] = {0};
    esp_err_t ret = ezo_ph_get_slope(sensor, slope_data, sizeof(slope_data));
    
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Slope query failed");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "slope", slope_data);
    cJSON_AddNumberToObject(response, "address", address);

    const char *payload = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload);
    free((void*)payload);
    sensor_read_guard_release(&guard);
    return ESP_OK;
}

static esp_err_t api_sensor_ec_temp_comp_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/ec-temp-comp/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Temperature compensation only available for EC sensors");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *temp = cJSON_GetObjectItem(payload, "temp_c");
    if (temp == NULL || !cJSON_IsNumber(temp)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing temp_c value");
        return ESP_FAIL;
    }
    float target = (float)temp->valuedouble;
    cJSON_Delete(payload);
    free(raw);

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_ec_set_temperature_comp(sensor, target);
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set temperature compensation");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_ec_output_params_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/ec-output-params/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Output parameters only available for EC sensors");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ESP_OK;
    
    cJSON *ec = cJSON_GetObjectItem(payload, "ec");
    if (ec != NULL && cJSON_IsBool(ec)) {
        ret = ezo_ec_set_output_parameter(sensor, "EC", cJSON_IsTrue(ec));
    }
    
    if (ret == ESP_OK) {
        cJSON *tds = cJSON_GetObjectItem(payload, "tds");
        if (tds != NULL && cJSON_IsBool(tds)) {
            ret = ezo_ec_set_output_parameter(sensor, "TDS", cJSON_IsTrue(tds));
        }
    }
    
    if (ret == ESP_OK) {
        cJSON *s = cJSON_GetObjectItem(payload, "s");
        if (s != NULL && cJSON_IsBool(s)) {
            ret = ezo_ec_set_output_parameter(sensor, "S", cJSON_IsTrue(s));
        }
    }
    
    if (ret == ESP_OK) {
        cJSON *sg = cJSON_GetObjectItem(payload, "sg");
        if (sg != NULL && cJSON_IsBool(sg)) {
            ret = ezo_ec_set_output_parameter(sensor, "SG", cJSON_IsTrue(sg));
        }
    }

    cJSON_Delete(payload);
    free(raw);

    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set output parameters");
        return ESP_FAIL;
    }

    // Re-query sensor to get updated output parameter state
    char param_response[EZO_LARGEST_STRING] = {0};
    ret = ezo_sensor_send_command(sensor, "O,?", param_response, sizeof(param_response), EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.ec.param_ec = false;
        sensor->config.ec.param_tds = false;
        sensor->config.ec.param_s = false;
        sensor->config.ec.param_sg = false;
        
        char response_copy[EZO_LARGEST_STRING];
        strncpy(response_copy, param_response, sizeof(response_copy) - 1);
        response_copy[sizeof(response_copy) - 1] = '\0';
        
        char *param_token = strtok(response_copy, ",");
        int param_field = 0;
        
        while (param_token != NULL) {
            if (param_field > 0) {
                if (strcmp(param_token, "EC") == 0 || strcmp(param_token, "Cond") == 0) {
                    sensor->config.ec.param_ec = true;
                } else if (strcmp(param_token, "TDS") == 0) {
                    sensor->config.ec.param_tds = true;
                } else if (strcmp(param_token, "S") == 0 || strcmp(param_token, "Sal") == 0) {
                    sensor->config.ec.param_s = true;
                } else if (strcmp(param_token, "SG") == 0) {
                    sensor->config.ec.param_sg = true;
                }
            }
            param_token = strtok(NULL, ",");
            param_field++;
        }
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

/**
 * @brief Full-featured manual command console handler
 * 
 * Implements WhiteBox EZO Console-style command processing with:
 * - !scan: Scan and list all I2C EZO devices
 * - !help: Display help information
 * - <address>: Select active sensor by I2C address (1-127)
 * - <command>: Send command to active sensor
 */
static esp_err_t api_manual_command_handler(httpd_req_t *req)
{
    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(payload, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'command' string");
        return ESP_FAIL;
    }

    const char *command = cJSON_GetStringValue(cmd_item);
    if (!command) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty command");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    char response_text[1024] = {0};
    
    // Handle console commands starting with '!'
    if (command[0] == '!') {
        if (strcmp(command, "!help") == 0) {
            snprintf(response_text, sizeof(response_text),
                "KannaCloud EZO Console - Available Commands:\n"
                "  !help       - Show this help message\n"
                "  !scan       - Scan and list all EZO sensors on I2C bus\n"
                "  !cmd        - Show commands for selected sensor\n"
                "  <address>   - Select sensor by I2C address (e.g., 99)\n"
                "  <command>   - Send command to active sensor (e.g., R, I, Status)\n"
                "\n"
                "Examples:\n"
                "  !scan                  - Find all sensors\n"
                "  99                     - Select sensor at address 99\n"
                "  I                      - Get device info\n"
                "  R                      - Read sensor value\n"
                "  Cal,mid,7.00          - Calibrate pH mid-point\n"
                "  T,25.5                 - Set temperature compensation\n"
                "\n"
                "Note: Sensor must be selected before sending commands.");
            
            cJSON_AddStringToObject(root, "status", "success");
            cJSON_AddStringToObject(root, "command", command);
            cJSON_AddStringToObject(root, "response", response_text);
        }
        else if (strcmp(command, "!scan") == 0) {
            // Scan I2C bus and list all EZO sensors
            uint8_t sensor_count = sensor_manager_get_ezo_count();
            
            if (sensor_count == 0) {
                snprintf(response_text, sizeof(response_text),
                    "I2C Bus Scan Complete\n"
                    "---\n"
                    "No EZO sensors found.\n"
                    "---\n"
                    "Make sure sensors are:\n"
                    "  1. Properly powered\n"
                    "  2. Connected to I2C bus\n"
                    "  3. In I2C mode (not UART)\n"
                    "  4. Using unique addresses");
            } else {
                int offset = snprintf(response_text, sizeof(response_text),
                    "I2C Bus Scan Complete\n"
                    "---\n"
                    "%d EZO sensor(s) found:\n\n", sensor_count);
                
                for (uint8_t i = 0; i < sensor_count && offset < sizeof(response_text) - 100; i++) {
                    ezo_sensor_t *sensor = (ezo_sensor_t*)sensor_manager_get_ezo_sensor(i);
                    if (sensor != NULL) {
                        const char *type_name = sensor->config.type;
                        if (strcmp(type_name, EZO_TYPE_PH) == 0) type_name = "pH";
                        else if (strcmp(type_name, EZO_TYPE_EC) == 0) type_name = "Conductivity (EC)";
                        else if (strcmp(type_name, EZO_TYPE_DO) == 0) type_name = "Dissolved Oxygen (DO)";
                        else if (strcmp(type_name, EZO_TYPE_ORP) == 0) type_name = "ORP";
                        else if (strcmp(type_name, EZO_TYPE_RTD) == 0) type_name = "Temperature (RTD)";
                        else if (strcmp(type_name, EZO_TYPE_HUM) == 0) type_name = "Humidity (HUM)";
                        
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "  [%d] Address: 0x%02X (%d)\n"
                            "      Type: EZO-%s\n"
                            "      Firmware: %s\n"
                            "      Name: %s\n\n",
                            i + 1,
                            sensor->config.i2c_address,
                            sensor->config.i2c_address,
                            sensor->config.type,
                            sensor->config.firmware_version[0] ? sensor->config.firmware_version : "Unknown",
                            sensor->config.name[0] ? sensor->config.name : "(unnamed)");
                    }
                }
                
                offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                    "---\n"
                    "To use a sensor, type its address (e.g., '%d')", 
                    ((ezo_sensor_t*)sensor_manager_get_ezo_sensor(0))->config.i2c_address);
            }
            
            cJSON_AddStringToObject(root, "status", "success");
            cJSON_AddStringToObject(root, "command", command);
            cJSON_AddStringToObject(root, "response", response_text);
        }
        else if (strcmp(command, "!cmd") == 0) {
            // Show commands for selected sensor
            cJSON *addr_item = cJSON_GetObjectItem(payload, "address");
            uint8_t target_address = 0;
            
            if (addr_item && cJSON_IsNumber(addr_item)) {
                target_address = (uint8_t)cJSON_GetNumberValue(addr_item);
            }
            
            if (target_address == 0) {
                snprintf(response_text, sizeof(response_text),
                    "Error: No sensor selected\n"
                    "---\n"
                    "Please select a sensor first by typing its address.\n"
                    "Example: Type '99' to select sensor at address 99");
                cJSON_AddStringToObject(root, "status", "error");
            } else {
                ezo_sensor_t *sensor = find_sensor_by_address(target_address);
                if (sensor == NULL) {
                    snprintf(response_text, sizeof(response_text),
                        "Error: No sensor at address %d", target_address);
                    cJSON_AddStringToObject(root, "status", "error");
                } else {
                    // Build sensor-specific command list
                    int offset = snprintf(response_text, sizeof(response_text),
                        "EZO-%s Commands (Address %d)\n"
                        "---\n"
                        "Common Commands:\n"
                        "  R              - Read sensor value\n"
                        "  I              - Device information\n"
                        "  Status         - Device status\n"
                        "  Factory        - Factory reset\n"
                        "  Sleep          - Enter low power mode\n"
                        "  Name,<name>    - Set device name\n"
                        "  Name,?         - Query device name\n\n",
                        sensor->config.type, target_address);
                    
                    // Add sensor-specific commands
                    if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "pH-Specific Commands:\n"
                            "  Cal,mid,7.00   - Calibrate midpoint at pH 7.00\n"
                            "  Cal,low,4.00   - Calibrate low point at pH 4.00\n"
                            "  Cal,high,10.00 - Calibrate high point at pH 10.00\n"
                            "  Cal,?          - Query calibration status\n"
                            "  Cal,clear      - Clear calibration\n"
                            "  Export,?       - Export calibration data (pg. 49)\n"
                            "  Import,<data>  - Import calibration data (pg. 50)\n"
                            "  Slope,?        - Query probe slope (pg. 51)\n"
                            "  pHext,<0|1>    - Extended pH scale -1 to 15 (pg. 52)\n"
                            "  pHext,?        - Query extended scale status\n"
                            "  T,<temp>       - Temperature compensation (pg. 53)\n"
                            "  T,?            - Query temp compensation\n");
                    } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "EC-Specific Commands:\n"
                            "  Cal,dry        - Calibrate dry (pg. 49)\n"
                            "  Cal,low,12880  - Calibrate low point\n"
                            "  Cal,high,80000 - Calibrate high point\n"
                            "  Cal,?          - Query calibration status\n"
                            "  Cal,clear      - Clear calibration\n"
                            "  Export,?       - Export calibration data (pg. 51)\n"
                            "  Import,<data>  - Import calibration data (pg. 52)\n"
                            "  K,<value>      - Set probe K value (pg. 53)\n"
                            "  K,?            - Query probe K value\n"
                            "  O,EC,<0|1>     - Enable/disable EC output (pg. 55)\n"
                            "  O,TDS,<0|1>    - Enable/disable TDS output\n"
                            "  O,S,<0|1>      - Enable/disable salinity output\n"
                            "  O,SG,<0|1>     - Enable/disable sp. gravity output\n"
                            "  O,?            - Query output parameters\n"
                            "  TDS,<value>    - Set TDS conversion factor (pg. 50)\n"
                            "  TDS,?          - Query TDS conversion factor\n"
                            "  T,<temp>       - Temperature compensation (pg. 54)\n"
                            "  T,?            - Query temp compensation\n");
                    } else if (strcmp(sensor->config.type, EZO_TYPE_DO) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "DO-Specific Commands:\n"
                            "  Cal              - Calibrate at atmospheric O2\n"
                            "  Cal,0            - Calibrate at zero dissolved O2\n"
                            "  Cal,?            - Query calibration status\n"
                            "  Cal,clear        - Clear calibration\n"
                            "  T,<temp>         - Temperature compensation\n"
                            "  S,<salinity>     - Salinity compensation (ppt)\n"
                            "  P,<pressure>     - Pressure compensation (kPa)\n");
                    } else if (strcmp(sensor->config.type, EZO_TYPE_ORP) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "ORP-Specific Commands:\n"
                            "  Cal,<mV>       - Calibrate at specific mV value\n"
                            "  Cal,?          - Query calibration status\n"
                            "  Cal,clear      - Clear calibration\n");
                    } else if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "RTD-Specific Commands:\n"
                            "  Cal,<temp>     - Calibrate at known temperature (pg. 45)\n"
                            "  Cal,?          - Query calibration status\n"
                            "  Cal,clear      - Clear calibration\n"
                            "  D,<0|1>        - Enable/disable data logger (pg. 49)\n"
                            "  D,?            - Query data logger status\n"
                            "  Export,?       - Export calibration data (pg. 46)\n"
                            "  Import,<data>  - Import calibration data (pg. 47)\n"
                            "  M              - Memory recall (pg. 50)\n"
                            "  M,clear        - Clear memory\n"
                            "  S,c            - Set scale to Celsius (pg. 48)\n"
                            "  S,k            - Set scale to Kelvin\n"
                            "  S,f            - Set scale to Fahrenheit\n"
                            "  S,?            - Query temperature scale\n");
                    } else if (strcmp(sensor->config.type, EZO_TYPE_HUM) == 0) {
                        offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                            "HUM-Specific Commands:\n"
                            "  Tcal,<temp>    - Temperature calibration (pg. 44)\n"
                            "  Tcal,?         - Query temp calibration status\n"
                            "  Tcal,clear     - Clear temp calibration\n"
                            "  O,HUM,<0|1>    - Enable/disable humidity output\n"
                            "  O,T,<0|1>      - Enable/disable temp output\n"
                            "  O,Dew,<0|1>    - Enable/disable dew point output\n"
                            "  O,?            - Query output parameters (pg. 43)\n"
                            "  Auto,<0|1>     - Enable/disable auto monitor (pg. 42)\n"
                            "  Auto,?         - Query auto monitor status\n");
                    }
                    
                    offset += snprintf(response_text + offset, sizeof(response_text) - offset,
                        "\n---\n"
                        "💡 Tip: Commands are case-insensitive");
                    
                    cJSON_AddStringToObject(root, "status", "success");
                }
            }
            cJSON_AddStringToObject(root, "command", command);
            cJSON_AddStringToObject(root, "response", response_text);
        }
        else {
            snprintf(response_text, sizeof(response_text),
                "Unknown console command: %s\n"
                "Type '!help' for available commands.", command);
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "command", command);
            cJSON_AddStringToObject(root, "response", response_text);
        }
    }
    // Check if command is an I2C address (1-127)
    else {
        char *endptr = NULL;
        long addr = strtol(command, &endptr, 10);
        
        // If entire string is a valid number between 1-127, treat as address selection
        if (endptr != command && *endptr == '\0' && addr >= 1 && addr <= 127) {
            ezo_sensor_t *sensor = find_sensor_by_address((uint8_t)addr);
            if (sensor != NULL) {
                snprintf(response_text, sizeof(response_text),
                    "Connected to sensor at address %d (0x%02X)\n"
                    "---\n"
                    "Type: EZO-%s\n"
                    "Firmware: %s\n"
                    "Name: %s\n"
                    "---\n"
                    "You can now send commands directly (e.g., 'R' to read, 'I' for info)\n"
                    "💡 Tip: Type '!cmd' to see all available commands for this sensor\n"
                    "For calibration, refer to the EZO-%s datasheet",
                    (int)addr, (int)addr,
                    sensor->config.type,
                    sensor->config.firmware_version[0] ? sensor->config.firmware_version : "Unknown",
                    sensor->config.name[0] ? sensor->config.name : "(unnamed)",
                    sensor->config.type);
                
                cJSON_AddStringToObject(root, "status", "success");
                cJSON_AddStringToObject(root, "command", command);
                cJSON_AddStringToObject(root, "response", response_text);
                cJSON_AddNumberToObject(root, "selected_address", addr);
                cJSON_AddStringToObject(root, "sensor_type", sensor->config.type);
            } else {
                snprintf(response_text, sizeof(response_text),
                    "No sensor found at address %d (0x%02X)\n"
                    "---\n"
                    "Type '!scan' to see available sensors", (int)addr, (int)addr);
                cJSON_AddStringToObject(root, "status", "error");
                cJSON_AddStringToObject(root, "command", command);
                cJSON_AddStringToObject(root, "response", response_text);
            }
        }
        // Otherwise, treat as EZO command (needs address in context)
        else {
            // For direct commands, we need to know which sensor to send to
            // Check if "address" field is provided in JSON
            cJSON *addr_item = cJSON_GetObjectItem(payload, "address");
            uint8_t target_address = 0;
            
            if (addr_item && cJSON_IsNumber(addr_item)) {
                target_address = (uint8_t)cJSON_GetNumberValue(addr_item);
            }
            
            if (target_address == 0) {
                snprintf(response_text, sizeof(response_text),
                    "Error: No sensor selected\n"
                    "---\n"
                    "Please select a sensor first by typing its address,\n"
                    "or include 'address' field in JSON request.\n\n"
                    "Example: Type '99' to select sensor at address 99\n"
                    "Then send commands like 'R', 'I', etc.");
                cJSON_AddStringToObject(root, "status", "error");
                cJSON_AddStringToObject(root, "command", command);
                cJSON_AddStringToObject(root, "response", response_text);
            } else {
                ezo_sensor_t *sensor = find_sensor_by_address(target_address);
                if (sensor == NULL) {
                    snprintf(response_text, sizeof(response_text),
                        "Error: No sensor at address %d", target_address);
                    cJSON_AddStringToObject(root, "status", "error");
                    cJSON_AddStringToObject(root, "command", command);
                    cJSON_AddStringToObject(root, "response", response_text);
                } else {
                    // Send command to sensor
                    char ezo_response[EZO_LARGEST_STRING] = {0};
                    sensor_read_guard_t guard;
                    sensor_read_guard_acquire(&guard);
                    
                    // Use appropriate wait time based on command
                    uint32_t wait_ms = EZO_LONG_WAIT_MS;
                    if (command[0] == 'R' || command[0] == 'r' ||
                        strncmp(command, "Cal", 3) == 0 || strncmp(command, "cal", 3) == 0) {
                        wait_ms = EZO_LONG_WAIT_MS;  // 1000ms for readings and calibrations
                    } else {
                        wait_ms = 300;  // 300ms for other commands
                    }
                    
                    esp_err_t ret = ezo_sensor_send_command(sensor, command, ezo_response, 
                                                           sizeof(ezo_response), wait_ms);
                    sensor_read_guard_release(&guard);
                    
                    if (ret == ESP_OK) {
                        snprintf(response_text, sizeof(response_text),
                            "[%d] %s > %s\n< %s",
                            target_address, sensor->config.type, command,
                            ezo_response[0] ? ezo_response : "(no response)");
                        cJSON_AddStringToObject(root, "status", "success");
                        cJSON_AddStringToObject(root, "command", command);
                        cJSON_AddStringToObject(root, "response", response_text);
                        cJSON_AddNumberToObject(root, "address", target_address);
                        cJSON_AddStringToObject(root, "raw_response", ezo_response);
                    } else {
                        snprintf(response_text, sizeof(response_text),
                            "[%d] %s > %s\n< ERROR: Command failed",
                            target_address, sensor->config.type, command);
                        cJSON_AddStringToObject(root, "status", "error");
                        cJSON_AddStringToObject(root, "command", command);
                        cJSON_AddStringToObject(root, "response", response_text);
                        cJSON_AddNumberToObject(root, "address", target_address);
                    }
                }
            }
        }
    }

    cJSON_Delete(payload);
    free(raw);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sensor_manual_command_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/command/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(payload, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'command' string");
        return ESP_FAIL;
    }

    const char *command = cJSON_GetStringValue(cmd_item);
    if (!command || strlen(command) == 0) {
        cJSON_Delete(payload);
        free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty command");
        return ESP_FAIL;
    }

    // Send command and get response
    // Use long wait time for manual commands (especially R command needs time to measure)
    char response[EZO_LARGEST_STRING] = {0};
    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);
    esp_err_t ret = ezo_sensor_send_command(sensor, command, response, sizeof(response), EZO_LONG_WAIT_MS);
    sensor_read_guard_release(&guard);

    cJSON_Delete(payload);
    free(raw);

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "status", ret == ESP_OK ? "success" : "error");
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddStringToObject(root, "response", ret == ESP_OK ? response : "Command failed");
    cJSON_AddNumberToObject(root, "address", address);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sensor_hum_output_params_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/hum-output-params/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    if (strcmp(sensor->config.type, EZO_TYPE_HUM) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Output parameters only available for HUM sensors");
        return ESP_FAIL;
    }

    char *raw = NULL;
    cJSON *payload = parse_request_json_body(req, &raw);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ESP_OK;
    
    cJSON *hum = cJSON_GetObjectItem(payload, "hum");
    if (hum != NULL && cJSON_IsBool(hum)) {
        ret = ezo_hum_set_output_parameter(sensor, "HUM", cJSON_IsTrue(hum));
    }
    
    if (ret == ESP_OK) {
        cJSON *t = cJSON_GetObjectItem(payload, "t");
        if (t != NULL && cJSON_IsBool(t)) {
            ret = ezo_hum_set_output_parameter(sensor, "T", cJSON_IsTrue(t));
        }
    }
    
    if (ret == ESP_OK) {
        cJSON *dew = cJSON_GetObjectItem(payload, "dew");
        if (dew != NULL && cJSON_IsBool(dew)) {
            ret = ezo_hum_set_output_parameter(sensor, "DEW", cJSON_IsTrue(dew));
        }
    }

    cJSON_Delete(payload);
    free(raw);

    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set output parameters");
        return ESP_FAIL;
    }

    // Re-query sensor to get updated output parameter state
    char param_response[EZO_LARGEST_STRING] = {0};
    ret = ezo_sensor_send_command(sensor, "O,?", param_response, sizeof(param_response), EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.hum.param_hum = false;
        sensor->config.hum.param_t = false;
        sensor->config.hum.param_dew = false;
        
        char response_copy[EZO_LARGEST_STRING];
        strncpy(response_copy, param_response, sizeof(response_copy) - 1);
        response_copy[sizeof(response_copy) - 1] = '\0';
        
        char *param_token = strtok(response_copy, ",");
        int param_field = 0;
        
        while (param_token != NULL) {
            if (param_field > 0) {
                if (strcmp(param_token, "HUM") == 0) {
                    sensor->config.hum.param_hum = true;
                } else if (strcmp(param_token, "T") == 0) {
                    sensor->config.hum.param_t = true;
                } else if (strcmp(param_token, "DEW") == 0) {
                    sensor->config.hum.param_dew = true;
                }
            }
            param_token = strtok(NULL, ",");
            param_field++;
        }
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static esp_err_t api_sensor_memory_clear_handler(httpd_req_t *req)
{
    uint8_t address;
    if (!parse_sensor_address_from_uri(req->uri, "/api/sensors/memory-clear/", &address)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensor address");
        return ESP_FAIL;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);

    esp_err_t ret = ezo_sensor_memory_clear(sensor);
    
    if (ret != ESP_OK) {
        sensor_read_guard_release(&guard);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory clear failed");
        return ESP_FAIL;
    }

    esp_err_t resp = send_sensor_success_response(req, sensor);
    sensor_read_guard_release(&guard);
    return resp;
}

static const httpd_uri_t s_sensor_uris[] = {
    {.uri = "/api/device/info",             .method = HTTP_GET,  .handler = api_device_info_get_handler,       .user_ctx = NULL},
    {.uri = "/api/device/name",             .method = HTTP_POST, .handler = api_device_name_set_handler,       .user_ctx = NULL},
    {.uri = "/api/sensors",                 .method = HTTP_GET,  .handler = api_sensors_list_handler,          .user_ctx = NULL},
    {.uri = "/api/sensors/rescan",          .method = HTTP_POST, .handler = api_sensors_rescan_handler,        .user_ctx = NULL},
    {.uri = "/api/sensors/config",          .method = HTTP_POST, .handler = api_sensors_config_handler,        .user_ctx = NULL},
    {.uri = "/api/sensors/config/batch",    .method = HTTP_POST, .handler = api_sensors_config_batch_handler,  .user_ctx = NULL},
    {.uri = "/api/sensors/pause",           .method = HTTP_POST, .handler = api_sensors_pause_handler,         .user_ctx = NULL},
    {.uri = "/api/sensors/resume",          .method = HTTP_POST, .handler = api_sensors_resume_handler,        .user_ctx = NULL},
    {.uri = "/api/sensors/calibrate/*",     .method = HTTP_POST, .handler = api_sensor_calibrate_handler,      .user_ctx = NULL},
    {.uri = "/api/sensors/compensate/*",    .method = HTTP_POST, .handler = api_sensor_compensation_handler,   .user_ctx = NULL},
    {.uri = "/api/sensors/mode/*",          .method = HTTP_POST, .handler = api_sensor_mode_handler,           .user_ctx = NULL},
    {.uri = "/api/sensors/power/*",         .method = HTTP_POST, .handler = api_sensor_power_handler,          .user_ctx = NULL},
    {.uri = "/api/sensors/status/*",        .method = HTTP_GET,  .handler = api_sensor_status_handler,         .user_ctx = NULL},
    {.uri = "/api/sensors/sample/*",        .method = HTTP_GET,  .handler = api_sensor_sample_handler,         .user_ctx = NULL},
    {.uri = "/api/sensors/export/*",        .method = HTTP_GET,  .handler = api_sensor_export_handler,         .user_ctx = NULL},
    {.uri = "/api/sensors/import/*",        .method = HTTP_POST, .handler = api_sensor_import_handler,         .user_ctx = NULL},
    {.uri = "/api/sensors/find/*",          .method = HTTP_POST, .handler = api_sensor_find_handler,           .user_ctx = NULL},
    {.uri = "/api/sensors/device-status/*", .method = HTTP_GET,  .handler = api_sensor_device_status_handler,  .user_ctx = NULL},
    {.uri = "/api/sensors/slope/*",         .method = HTTP_GET,  .handler = api_sensor_slope_handler,          .user_ctx = NULL},
    {.uri = "/api/sensors/ec-temp-comp/*",  .method = HTTP_POST, .handler = api_sensor_ec_temp_comp_handler,   .user_ctx = NULL},
    {.uri = "/api/sensors/ec-output-params/*", .method = HTTP_POST, .handler = api_sensor_ec_output_params_handler, .user_ctx = NULL},
    {.uri = "/api/sensors/hum-output-params/*", .method = HTTP_POST, .handler = api_sensor_hum_output_params_handler, .user_ctx = NULL},
    {.uri = "/api/sensors/command/*",       .method = HTTP_POST, .handler = api_sensor_manual_command_handler, .user_ctx = NULL},
    {.uri = "/api/manual-command",          .method = HTTP_POST, .handler = api_manual_command_handler,        .user_ctx = NULL},
    {.uri = "/api/sensors/memory-clear/*",  .method = HTTP_POST, .handler = api_sensor_memory_clear_handler,   .user_ctx = NULL},
};

esp_err_t http_handlers_sensors_init(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(s_sensor_uris) / sizeof(s_sensor_uris[0]); ++i) {
        const httpd_uri_t *uri = &s_sensor_uris[i];
        esp_err_t err = httpd_register_uri_handler(server, uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register sensor URI %s: %s", uri->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Registered %d sensor HTTP endpoints", (int)(sizeof(s_sensor_uris) / sizeof(s_sensor_uris[0])));
    return ESP_OK;
}

void http_handlers_sensors_deinit(void)
{
    ESP_LOGI(TAG, "Sensor handlers deinit noop");
}
