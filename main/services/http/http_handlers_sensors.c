#include "http_handlers_sensors.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "HTTP_SENSORS";
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

esp_err_t http_handlers_sensors_init(httpd_handle_t server)
{
    (void)server;
    ESP_LOGI(TAG, "Sensor handlers init placeholder");
    return ESP_OK;
}

void http_handlers_sensors_deinit(void)
{
    ESP_LOGI(TAG, "Sensor handlers deinit placeholder");
}
