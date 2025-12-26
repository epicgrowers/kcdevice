/**
 * @file http_websocket.c
 * @brief WebSocket handlers for real-time sensor data streaming
 */

#include "http_websocket.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "ezo_sensor.h"
#include <string.h>

static const char *TAG = "HTTP_WS";

#define SENSOR_WS_MAX_CLIENTS  4
#define FOCUS_SAMPLE_INTERVAL_MS 2000

typedef struct {
    int fd;
    bool active;
} sensor_ws_client_t;

typedef struct {
    httpd_handle_t server;
    int fd;
    char *payload;
    size_t len;
} ws_async_send_arg_t;

// Module state
static httpd_handle_t s_server = NULL;
static sensor_ws_client_t s_ws_clients[SENSOR_WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_clients_mutex = NULL;
static esp_timer_handle_t s_focus_timer = NULL;
static bool s_focus_stream_active = false;
static uint8_t s_focus_sensor_address = 0;
static bool s_focus_paused_readings = false;
static SemaphoreHandle_t s_sensor_guard_mutex = NULL;

// Forward declarations
static void sensor_ws_remove_client(int fd);
static void sensor_ws_send_json_to_client(int fd, const char *json, size_t len);
static void sensor_ws_broadcast_json(const char *json, size_t len);
static void sensor_ws_send_snapshot_to_client(int fd);
static void handle_ws_command(int client_fd, const char *payload, size_t len);
static esp_err_t focus_stream_start(uint8_t address);
static void focus_stream_stop(void);
static void focus_stream_sample_now(void);
static void focus_timer_cb(void *arg);
static ezo_sensor_t *find_sensor_by_address(uint8_t address);
static cJSON *create_sensors_object_from_cache(const sensor_cache_t *cache);
static cJSON *build_sensor_json(ezo_sensor_t *sensor, int index, bool include_runtime);
static void add_sample_readings_to_json(cJSON *json, const char *type, const float values[], uint8_t count);
static void sensor_ws_send_focus_status(const char *status, uint8_t address);
static void sensor_ws_send_focus_sample(ezo_sensor_t *sensor, const float *values, uint8_t count);
static void wait_for_sensor_reading_idle(void);

// Sensor read guard helpers
typedef struct {
    bool should_resume;
    bool mutex_acquired;
} sensor_read_guard_t;

static void sensor_read_guard_acquire(sensor_read_guard_t *guard)
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
            guard->mutex_acquired = false;
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

static void sensor_read_guard_release(sensor_read_guard_t *guard)
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

// WebSocket async send callback
static void ws_send_work_cb(void *arg)
{
    ws_async_send_arg_t *resp = (ws_async_send_arg_t *)arg;
    if (resp == NULL) {
        return;
    }

    if (resp->server != NULL) {
        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)resp->payload,
            .len = resp->len
        };

        esp_err_t ret = httpd_ws_send_frame_async(resp->server, resp->fd, &frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send WS frame to fd %d: %s", resp->fd, esp_err_to_name(ret));
            sensor_ws_remove_client(resp->fd);
        }
    }

    if (resp->payload != NULL) {
        free(resp->payload);
    }
    free(resp);
}

// Client management
static void sensor_ws_ensure_mutex(void)
{
    if (s_ws_clients_mutex == NULL) {
        s_ws_clients_mutex = xSemaphoreCreateMutex();
        if (s_ws_clients_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create WS client mutex");
        }
    }
}

static void sensor_ws_add_client(int fd)
{
    sensor_ws_ensure_mutex();
    if (s_ws_clients_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool added = false;
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (!s_ws_clients[i].active) {
                s_ws_clients[i].active = true;
                s_ws_clients[i].fd = fd;
                ESP_LOGI(TAG, "WS client added (fd=%d, slot=%d)", fd, i);
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_clients_mutex);

        if (!added) {
            ESP_LOGW(TAG, "WS client limit reached, closing fd %d", fd);
            httpd_sess_trigger_close(s_server, fd);
        }
    }
}

static void sensor_ws_remove_client(int fd)
{
    if (s_ws_clients_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active && s_ws_clients[i].fd == fd) {
                ESP_LOGI(TAG, "WS client removed (fd=%d)", fd);
                s_ws_clients[i].active = false;
                s_ws_clients[i].fd = -1;
                break;
            }
        }
        bool any_active = false;
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active) {
                any_active = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_clients_mutex);

        if (!any_active && s_focus_stream_active) {
            ESP_LOGI(TAG, "No WS clients connected, stopping focus stream");
            focus_stream_stop();
        }
    }
}

static bool sensor_ws_has_clients(void)
{
    if (s_ws_clients_mutex == NULL) {
        return false;
    }
    bool result = false;
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active) {
                result = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_clients_mutex);
    }
    return result;
}

// WebSocket send functions
static void sensor_ws_send_json_to_client(int fd, const char *json, size_t len)
{
    if (s_server == NULL || json == NULL || len == 0) {
        return;
    }

    ws_async_send_arg_t *arg = calloc(1, sizeof(ws_async_send_arg_t));
    if (arg == NULL) {
        return;
    }
    arg->payload = malloc(len);
    if (arg->payload == NULL) {
        free(arg);
        return;
    }
    memcpy(arg->payload, json, len);
    arg->len = len;
    arg->fd = fd;
    arg->server = s_server;

    if (httpd_queue_work(s_server, ws_send_work_cb, arg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue WS send work");
        free(arg->payload);
        free(arg);
    }
}

static void sensor_ws_broadcast_json(const char *json, size_t len)
{
    if (json == NULL || len == 0 || s_server == NULL) {
        return;
    }
    if (!sensor_ws_has_clients()) {
        return;
    }

    if (s_ws_clients_mutex == NULL) {
        return;
    }

    int targets[SENSOR_WS_MAX_CLIENTS];
    size_t target_count = 0;
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active && target_count < SENSOR_WS_MAX_CLIENTS) {
                targets[target_count++] = s_ws_clients[i].fd;
            }
        }
        xSemaphoreGive(s_ws_clients_mutex);
    }

    for (size_t i = 0; i < target_count; i++) {
        sensor_ws_send_json_to_client(targets[i], json, len);
    }
}

// Helper functions for sensor data
static ezo_sensor_t *find_sensor_by_address(uint8_t address)
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

static void add_sample_readings_to_json(cJSON *json, const char *type, const float values[], uint8_t count)
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

static void add_capabilities_array(cJSON *parent, uint32_t flags)
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

static void append_sensor_runtime_info(cJSON *json, ezo_sensor_t *sensor)
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

static cJSON *build_sensor_json(ezo_sensor_t *sensor, int index, bool include_runtime)
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

static cJSON *create_sensors_object_from_cache(const sensor_cache_t *cache)
{
    cJSON *sensors = cJSON_CreateObject();
    if (sensors == NULL || cache == NULL) {
        return sensors;
    }

    for (uint8_t i = 0; i < cache->sensor_count && i < 8; i++) {
        const cached_sensor_t *sensor = &cache->sensors[i];
        if (!sensor->valid) {
            continue;
        }

        if (sensor->value_count == 1) {
            cJSON_AddNumberToObject(sensors, sensor->sensor_type, sensor->values[0]);
        } else if (sensor->value_count > 1) {
            cJSON *sensor_obj = cJSON_CreateObject();
            if (sensor_obj == NULL) {
                continue;
            }

            if (strcmp(sensor->sensor_type, "HUM") == 0) {
                if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "humidity", sensor->values[0]);
                if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "air_temp", sensor->values[1]);
                if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "dew_point", sensor->values[2]);
            } else if (strcmp(sensor->sensor_type, "EC") == 0) {
                if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "conductivity", sensor->values[0]);
                if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "tds", sensor->values[1]);
                if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "salinity", sensor->values[2]);
                if (sensor->value_count >= 4) cJSON_AddNumberToObject(sensor_obj, "specific_gravity", sensor->values[3]);
            } else if (strcmp(sensor->sensor_type, "DO") == 0) {
                if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "dissolved_oxygen", sensor->values[0]);
                if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "saturation", sensor->values[1]);
            } else {
                for (uint8_t j = 0; j < sensor->value_count; j++) {
                    char field_name[16];
                    snprintf(field_name, sizeof(field_name), "value_%d", j);
                    cJSON_AddNumberToObject(sensor_obj, field_name, sensor->values[j]);
                }
            }

            cJSON_AddItemToObject(sensors, sensor->sensor_type, sensor_obj);
        }
    }

    return sensors;
}

// Status snapshot functions
static void sensor_ws_emit_status_payload(const sensor_cache_t *cache, int target_fd)
{
    if (cache == NULL) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "status_snapshot");
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)(cache->timestamp_us / 1000ULL));
    if (cache->battery_valid) {
        cJSON_AddNumberToObject(root, "battery", cache->battery_percentage);
    }
    cJSON_AddNumberToObject(root, "rssi", cache->rssi);

    cJSON *sensors = create_sensors_object_from_cache(cache);
    if (sensors != NULL) {
        cJSON_AddItemToObject(root, "sensors", sensors);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json != NULL) {
        if (target_fd >= 0) {
            sensor_ws_send_json_to_client(target_fd, json, strlen(json));
        } else {
            sensor_ws_broadcast_json(json, strlen(json));
        }
        free(json);
    }
}

static void sensor_ws_send_snapshot_to_client(int fd)
{
    sensor_cache_t cache;
    if (sensor_manager_get_cached_data(&cache) == ESP_OK) {
        sensor_ws_emit_status_payload(&cache, fd);
    }
}

// Focus mode functions
static void sensor_ws_send_focus_status(const char *status, uint8_t address)
{
    if (status == NULL) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "focus_status");
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "address", address);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json != NULL) {
        sensor_ws_broadcast_json(json, strlen(json));
        free(json);
    }
}

static void sensor_ws_send_focus_sample(ezo_sensor_t *sensor, const float *values, uint8_t count)
{
    if (sensor == NULL || values == NULL || count == 0) {
        return;
    }

    cJSON *sensor_json = build_sensor_json(sensor, -1, true);
    if (sensor_json == NULL) {
        return;
    }

    uint64_t timestamp_ms = esp_timer_get_time() / 1000ULL;
    cJSON_AddNumberToObject(sensor_json, "timestamp_ms", (double)timestamp_ms);
    cJSON_AddNumberToObject(sensor_json, "value_count", count);

    cJSON *raw = cJSON_CreateArray();
    if (raw != NULL) {
        for (uint8_t i = 0; i < count; i++) {
            cJSON_AddItemToArray(raw, cJSON_CreateNumber(values[i]));
        }
        cJSON_AddItemToObject(sensor_json, "raw", raw);
    }

    add_sample_readings_to_json(sensor_json, sensor->config.type, values, count);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(sensor_json);
        return;
    }

    cJSON_AddStringToObject(root, "type", "focus_sample");
    cJSON_AddItemToObject(root, "sensor", sensor_json);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json != NULL) {
        sensor_ws_broadcast_json(json, strlen(json));
        free(json);
    }
}

static void wait_for_sensor_reading_idle(void)
{
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

static void focus_stream_sample_now(void)
{
    if (!s_focus_stream_active) {
        return;
    }

    ezo_sensor_t *sensor = find_sensor_by_address(s_focus_sensor_address);
    if (sensor == NULL) {
        ESP_LOGW(TAG, "Focus sensor 0x%02X not found", s_focus_sensor_address);
        focus_stream_stop();
        return;
    }

    float values[4] = {0};
    uint8_t count = 0;

    sensor_read_guard_t guard;
    sensor_read_guard_acquire(&guard);
    esp_err_t ret = ezo_sensor_read_all(sensor, values, &count);
    sensor_read_guard_release(&guard);

    if (ret == ESP_OK && count > 0) {
        sensor_ws_send_focus_sample(sensor, values, count);
    } else {
        ESP_LOGW(TAG, "Focus read failed for 0x%02X: %s", sensor->config.i2c_address, esp_err_to_name(ret));
    }
}

static void focus_timer_cb(void *arg)
{
    (void)arg;
    focus_stream_sample_now();
}

static esp_err_t focus_stream_start(uint8_t address)
{
    ezo_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_focus_stream_active) {
        s_focus_sensor_address = address;
        focus_stream_sample_now();
        sensor_ws_send_focus_status("started", address);
        return ESP_OK;
    }

    if (!sensor_manager_is_reading_paused()) {
        sensor_manager_pause_reading();
        s_focus_paused_readings = true;
    } else {
        s_focus_paused_readings = false;
    }

    wait_for_sensor_reading_idle();

    s_focus_sensor_address = address;
    s_focus_stream_active = true;

    if (s_focus_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = focus_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "focus_stream"
        };
        if (esp_timer_create(&args, &s_focus_timer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create focus stream timer");
            s_focus_stream_active = false;
            return ESP_FAIL;
        }
    } else {
        esp_timer_stop(s_focus_timer);
    }

    focus_stream_sample_now();

    esp_timer_start_periodic(s_focus_timer, FOCUS_SAMPLE_INTERVAL_MS * 1000ULL);
    sensor_ws_send_focus_status("started", address);
    return ESP_OK;
}

static void focus_stream_stop(void)
{
    if (s_focus_timer != NULL) {
        esp_timer_stop(s_focus_timer);
    }

    if (!s_focus_stream_active && !s_focus_paused_readings) {
        return;
    }

    uint8_t last_address = s_focus_sensor_address;
    s_focus_stream_active = false;
    s_focus_sensor_address = 0;

    if (s_focus_paused_readings) {
        sensor_manager_resume_reading();
        s_focus_paused_readings = false;
    }

    sensor_ws_send_focus_status("stopped", last_address);
}

// WebSocket command handler
static void handle_ws_command(int client_fd, const char *payload, size_t len)
{
    if (payload == NULL || len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid WS payload");
        return;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action) && action->valuestring != NULL) {
        if (strcmp(action->valuestring, "request_snapshot") == 0) {
            sensor_ws_send_snapshot_to_client(client_fd);
        } else if (strcmp(action->valuestring, "focus_start") == 0) {
            cJSON *addr = cJSON_GetObjectItem(root, "address");
            if (cJSON_IsNumber(addr)) {
                uint8_t address = (uint8_t)addr->valueint;
                esp_err_t ret = focus_stream_start(address);
                if (ret != ESP_OK) {
                    sensor_ws_send_focus_status("error", address);
                }
            }
        } else if (strcmp(action->valuestring, "focus_stop") == 0) {
            focus_stream_stop();
        }
    }

    cJSON_Delete(root);
}

// Public API implementation
esp_err_t http_websocket_init(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_server = server;

    // Initialize client tracking
    sensor_ws_ensure_mutex();
    for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
        s_ws_clients[i].active = false;
        s_ws_clients[i].fd = -1;
    }

    // Register WebSocket URI handler
    httpd_uri_t sensor_ws_uri = {
        .uri       = "/ws/sensors",
        .method    = HTTP_GET,
        .handler   = sensor_ws_handler,
        .user_ctx  = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true
    };
    
    esp_err_t ret = httpd_register_uri_handler(server, &sensor_ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket subsystem initialized");
    return ESP_OK;
}

void http_websocket_deinit(void)
{
    // Stop focus stream
    focus_stream_stop();

    // Delete timer
    if (s_focus_timer != NULL) {
        esp_timer_delete(s_focus_timer);
        s_focus_timer = NULL;
    }

    // Clear client connections
    if (s_ws_clients_mutex != NULL) {
        xSemaphoreTake(s_ws_clients_mutex, portMAX_DELAY);
        for (int i = 0; i < SENSOR_WS_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active && s_server != NULL) {
                httpd_sess_trigger_close(s_server, s_ws_clients[i].fd);
            }
            s_ws_clients[i].active = false;
            s_ws_clients[i].fd = -1;
        }
        xSemaphoreGive(s_ws_clients_mutex);
        vSemaphoreDelete(s_ws_clients_mutex);
        s_ws_clients_mutex = NULL;
    }

    if (s_sensor_guard_mutex != NULL) {
        vSemaphoreDelete(s_sensor_guard_mutex);
        s_sensor_guard_mutex = NULL;
    }

    s_server = NULL;
    ESP_LOGI(TAG, "WebSocket subsystem deinitialized");
}

esp_err_t sensor_ws_handler(httpd_req_t *req)
{
    int client_fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        sensor_ws_add_client(client_fd);
        sensor_ws_send_snapshot_to_client(client_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .final = true,
        .fragmented = false,
        .payload = NULL,
        .len = 0
    };

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frame.len > 0) {
        frame.payload = malloc(frame.len + 1);
        if (frame.payload == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(frame.payload);
            return ret;
        }
        frame.payload[frame.len] = '\0';
    }

    if (frame.type == HTTPD_WS_TYPE_TEXT && frame.payload != NULL) {
        handle_ws_command(client_fd, (const char *)frame.payload, frame.len);
    } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        sensor_ws_remove_client(client_fd);
    } else if (frame.type == HTTPD_WS_TYPE_PING) {
        frame.type = HTTPD_WS_TYPE_PONG;
        httpd_ws_send_frame(req, &frame);
    }

    if (frame.payload != NULL) {
        free(frame.payload);
    }

    return ESP_OK;
}

void http_websocket_cache_update_handler(const sensor_cache_t *cache, void *ctx)
{
    (void)ctx;
    if (cache != NULL) {
        sensor_ws_emit_status_payload(cache, -1);
    }
}
