/**
 * @file mqtt_telemetry.c
 * @brief MQTT client implementation for cloud telemetry
 */

#include "mqtt_telemetry.h"

#include "services/core/services.h"
#include "services/core/services_provisioning_bridge.h"
#include "services/telemetry/mqtt_connection_controller.h"
#include "services/telemetry/mqtt_connection_watchdog.h"
#include "services/telemetry/mqtt_payload_builder.h"
#include "services/telemetry/mqtt_publish_loop.h"
#include "services/telemetry/telemetry_pipeline.h"

#include "sensors/drivers/ezo_sensor.h"
#include "sensors/pipeline.h"
#include "sensors/sensor_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MQTT_DEFAULT_PUBLISH_INTERVAL_SEC (10U)
#define MQTT_DEFAULT_BUSY_BACKOFF_MS      (1000U)
#define MQTT_DEFAULT_CLIENT_BACKOFF_MS    (750U)
#define MQTT_DEFAULT_IDLE_DELAY_MS        (200U)

static const char *TAG = "MQTT_TELEM";

static TaskHandle_t s_publish_task_handle = NULL;
static uint32_t s_publish_interval_sec = MQTT_DEFAULT_PUBLISH_INTERVAL_SEC;
static char s_device_id[SERVICES_DEVICE_ID_MAX_LEN] = {0};
static char s_device_name[SERVICES_DEVICE_NAME_MAX_LEN] = {0};
static char s_mqtt_ca_cert[SERVICES_MQTT_CA_MAX_BYTES] = {0};

static mqtt_publish_scheduler_t s_publish_scheduler = {0};
static mqtt_publish_loop_t s_publish_loop = {0};
static const mqtt_publish_loop_config_t s_publish_loop_defaults = {
    .busy_backoff_ms = MQTT_DEFAULT_BUSY_BACKOFF_MS,
    .client_backoff_ms = MQTT_DEFAULT_CLIENT_BACKOFF_MS,
    .idle_delay_ms = MQTT_DEFAULT_IDLE_DELAY_MS,
};
static mqtt_publish_loop_config_t s_publish_loop_config = {
    .busy_backoff_ms = MQTT_DEFAULT_BUSY_BACKOFF_MS,
    .client_backoff_ms = MQTT_DEFAULT_CLIENT_BACKOFF_MS,
    .idle_delay_ms = MQTT_DEFAULT_IDLE_DELAY_MS,
};
static telemetry_pipeline_t s_telemetry_pipeline = {0};

static void mqtt_publish_task(void *arg);
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data);
static void mqtt_handle_command_payload(const char *payload, int length);
static void mqtt_load_saved_interval(void);
static esp_err_t mqtt_fetch_ca_certificate(char *buffer, size_t *out_length);
static void mqtt_log_certificate_preview(size_t length);
static esp_err_t mqtt_acquire_sample(telemetry_pipeline_sample_t *sample);
static void mqtt_populate_sensor_data_from_cache(const sensor_cache_t *cache,
                                                sensor_data_t *out_sensors);

static void mqtt_load_saved_interval(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to open NVS for MQTT interval (%s), using default %lus",
                 esp_err_to_name(err),
                 (unsigned long)s_publish_interval_sec);
        return;
    }

    uint32_t saved_interval = s_publish_interval_sec;
    err = nvs_get_u32(nvs_handle, "mqtt_interval", &saved_interval);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        s_publish_interval_sec = saved_interval;
        ESP_LOGI(TAG,
                 "Loaded MQTT interval from NVS: %lu seconds",
                 (unsigned long)saved_interval);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "Failed to load MQTT interval (%s), using %lu seconds",
                 esp_err_to_name(err),
                 (unsigned long)s_publish_interval_sec);
    }
}

static esp_err_t mqtt_fetch_ca_certificate(char *buffer, size_t *out_length)
{
    if (buffer == NULL || out_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ca_len = 0;
    esp_err_t ret = services_provisioning_load_mqtt_ca_certificate(buffer, &ca_len);
    if (ret != ESP_OK) {
        buffer[0] = '\0';
        *out_length = 0;
        return ret;
    }

    if (ca_len >= SERVICES_MQTT_CA_MAX_BYTES) {
        ca_len = SERVICES_MQTT_CA_MAX_BYTES - 1;
    }
    buffer[ca_len] = '\0';
    *out_length = ca_len;
    return ESP_OK;
}

static void mqtt_log_certificate_preview(size_t length)
{
    if (length == 0) {
        return;
    }

    size_t preview_len = (length < 149U) ? length : 149U;
    char preview_start[150];
    memcpy(preview_start, s_mqtt_ca_cert, preview_len);
    preview_start[preview_len] = '\0';
    ESP_LOGI(TAG, "Certificate START: %s", preview_start);

    if (length > 149U) {
        size_t tail_len = (length < 100U) ? length : 100U;
        char preview_end[101];
        memcpy(preview_end, s_mqtt_ca_cert + (length - tail_len), tail_len);
        preview_end[tail_len] = '\0';
        ESP_LOGI(TAG, "Certificate END: %s", preview_end);
    }
}

static void mqtt_handle_command_payload(const char *payload, int length)
{
    if (payload == NULL || length <= 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, (size_t)length);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse MQTT command payload");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (cJSON_IsString(cmd) && cmd->valuestring != NULL) {
        ESP_LOGI(TAG, "MQTT command received: %s", cmd->valuestring);
        if (strcmp(cmd->valuestring, "reboot") == 0) {
            mqtt_publish_status("rebooting");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else if (strcmp(cmd->valuestring, "ping") == 0) {
            mqtt_publish_status("pong");
        }
    }

    cJSON_Delete(root);
}

static esp_err_t mqtt_acquire_sample(telemetry_pipeline_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return telemetry_pipeline_acquire_sample(&s_telemetry_pipeline, sample);
}

static void mqtt_populate_sensor_data_from_cache(const sensor_cache_t *cache,
                                                sensor_data_t *out_sensors)
{
    if (cache == NULL || out_sensors == NULL) {
        return;
    }

    out_sensors->temperature = NAN;
    out_sensors->humidity = NAN;
    out_sensors->soil_moisture = NAN;
    out_sensors->light_level = NAN;
    out_sensors->ph = NAN;
    out_sensors->ec = NAN;
    out_sensors->dissolved_oxygen = NAN;
    out_sensors->orp = NAN;

    for (uint8_t i = 0; i < cache->sensor_count && i < 8; i++) {
        const cached_sensor_t *sensor = &cache->sensors[i];
        if (!sensor->valid) {
            continue;
        }

        if (strcmp(sensor->sensor_type, "RTD") == 0 && sensor->value_count >= 1) {
            out_sensors->temperature = sensor->values[0];
        } else if (strcmp(sensor->sensor_type, "HUM") == 0) {
            if (sensor->value_count >= 1) {
                out_sensors->humidity = sensor->values[0];
            }
            if (sensor->value_count >= 2) {
                out_sensors->temperature = sensor->values[1];
            }
        } else if (strcmp(sensor->sensor_type, "EC") == 0 && sensor->value_count >= 1) {
            out_sensors->ec = sensor->values[0];
        } else if (strcmp(sensor->sensor_type, "DO") == 0 && sensor->value_count >= 1) {
            out_sensors->dissolved_oxygen = sensor->values[0];
        } else if (strcmp(sensor->sensor_type, "ORP") == 0 && sensor->value_count >= 1) {
            out_sensors->orp = sensor->values[0];
        } else if (strcmp(sensor->sensor_type, "PH") == 0 && sensor->value_count >= 1) {
            out_sensors->ph = sensor->values[0];
        }
    }
}

static void mqtt_publish_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "MQTT publish task running (interval=%lus)",
             (unsigned long)s_publish_interval_sec);

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        mqtt_publish_loop_inputs_t inputs = {
            .mqtt_connected = mqtt_connection_controller_is_connected(),
            .sensor_reading_in_progress = sensor_manager_is_reading_in_progress(),
            .sensor_reading_paused = sensor_manager_is_reading_paused(),
        };

        mqtt_publish_loop_decision_t decision =
            mqtt_publish_loop_next_action(&s_publish_loop, &inputs, now_us);

        if (decision.action == MQTT_PUBLISH_LOOP_ACTION_BLOCK) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (decision.action == MQTT_PUBLISH_LOOP_ACTION_WAIT) {
            uint32_t wait_ms = decision.wait_ms;
            if (wait_ms == 0) {
                wait_ms = mqtt_publish_loop_get_idle_delay(&s_publish_loop);
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        telemetry_pipeline_sample_t sample = {0};
        esp_err_t sample_ret = mqtt_acquire_sample(&sample);
        if (sample_ret != ESP_OK || !sample.cache_valid) {
            ESP_LOGW(TAG, "Sensor snapshot unavailable - delaying publish");
            mqtt_publish_loop_handle_result(&s_publish_loop,
                                            MQTT_PUBLISH_RESULT_SNAPSHOT_UNAVAILABLE,
                                            esp_timer_get_time());
            continue;
        }

        const sensor_pipeline_snapshot_t *snapshot = &sample.snapshot;

        mqtt_payload_context_t payload_ctx = {
            .device_id = s_device_id,
            .device_name = (s_device_name[0] != '\0') ? s_device_name : NULL,
        };

        char *json_str = NULL;
        esp_err_t payload_ret = mqtt_payload_build(&payload_ctx, snapshot, &json_str);
        if (payload_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to build MQTT payload (%s)", esp_err_to_name(payload_ret));
            mqtt_publish_loop_handle_result(&s_publish_loop,
                                            MQTT_PUBLISH_RESULT_PAYLOAD_ERROR,
                                            esp_timer_get_time());
            continue;
        }

        esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
        bool client_connected = mqtt_connection_controller_is_connected();
        const char *connection_note = "client_unavailable";
        if (client != NULL) {
            connection_note = client_connected ? "connected" : "disconnected";
        }
        telemetry_pipeline_log_publish_attempt(&sample, "mqtt", connection_note);

        if (client == NULL || !client_connected) {
            telemetry_pipeline_record_publish_result(&s_telemetry_pipeline,
                                                    &sample,
                                                    false);
            free(json_str);
            mqtt_publish_loop_handle_result(&s_publish_loop,
                                            MQTT_PUBLISH_RESULT_CLIENT_UNAVAILABLE,
                                            esp_timer_get_time());
            continue;
        }

        ESP_LOGI(TAG, "Publishing MQTT payload: %s", json_str);

        char topic[128];
        snprintf(topic, sizeof(topic), "kannacloud/sensor/%s/data", s_device_id);

        int msg_id = esp_mqtt_client_publish(client, topic, json_str, 0, 1, 0);
        free(json_str);

        bool publish_success = (msg_id >= 0);
        telemetry_pipeline_record_publish_result(&s_telemetry_pipeline,
                                                &sample,
                                                publish_success);

        if (publish_success) {
            ESP_LOGI(TAG, "MQTT data published successfully (msg_id=%d)", msg_id);
            mqtt_publish_loop_handle_result(&s_publish_loop,
                                            MQTT_PUBLISH_RESULT_SUCCESS,
                                            esp_timer_get_time());
        } else {
            ESP_LOGW(TAG, "MQTT publish failed (msg_id=%d)", msg_id);
            mqtt_publish_loop_handle_result(&s_publish_loop,
                                            MQTT_PUBLISH_RESULT_TEMPORARY_FAILURE,
                                            esp_timer_get_time());
        }
    }
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Connected to MQTT broker");
            mqtt_connection_controller_handle_connected();

            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "kannacloud/sensor/%s/cmd", s_device_id);
            esp_mqtt_client_subscribe(event->client, cmd_topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s", cmd_topic);

            mqtt_publish_status("online");
            mqtt_publish_scheduler_request_publish(&s_publish_scheduler);
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            mqtt_connection_controller_handle_disconnected();
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred (type=%d)", event->error_handle->error_type);
            mqtt_connection_controller_handle_error();
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG,
                     "Received message on %.*s",
                     event->topic_len,
                     event->topic);
            mqtt_handle_command_payload(event->data, event->data_len);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribed (msg_id=%d)", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Unsubscribed (msg_id=%d)", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Publish acknowledged (msg_id=%d)", event->msg_id);
            break;
        default:
            break;
    }
}

esp_err_t mqtt_client_init(const mqtt_telemetry_config_t *config)
{
    if (config == NULL || config->broker_uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mqtt_connection_controller_get_client() != NULL) {
        ESP_LOGW(TAG, "MQTT client already initialized");
        return ESP_OK;
    }

    if (!services_provisioning_bridge_has_device_identity()) {
        ESP_LOGE(TAG, "Provisioning bridge missing device identity");
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_load_saved_interval();
    if (config->publish_interval_sec > 0) {
        s_publish_interval_sec = config->publish_interval_sec;
        ESP_LOGI(TAG,
                 "MQTT interval override applied: %lu seconds",
                 (unsigned long)s_publish_interval_sec);
    }

    telemetry_pipeline_config_t pipeline_cfg = {
        .sensor_ctx = config->sensor_ctx,
    };
    telemetry_pipeline_init(&s_telemetry_pipeline, &pipeline_cfg);

    s_publish_loop_config = s_publish_loop_defaults;
    if (config->busy_backoff_ms > 0) {
        s_publish_loop_config.busy_backoff_ms = config->busy_backoff_ms;
    }
    if (config->client_backoff_ms > 0) {
        s_publish_loop_config.client_backoff_ms = config->client_backoff_ms;
    }
    if (config->idle_delay_ms > 0) {
        s_publish_loop_config.idle_delay_ms = config->idle_delay_ms;
    }
    ESP_LOGI(TAG,
             "MQTT loop tuning: busy=%lums, client=%lums, idle=%lums",
             (unsigned long)s_publish_loop_config.busy_backoff_ms,
             (unsigned long)s_publish_loop_config.client_backoff_ms,
             (unsigned long)s_publish_loop_config.idle_delay_ms);

    mqtt_publish_scheduler_init(&s_publish_scheduler, s_publish_interval_sec);
    mqtt_publish_loop_init(&s_publish_loop, &s_publish_scheduler, &s_publish_loop_config);

    if (services_provisioning_load_device_id(s_device_id, sizeof(s_device_id)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load device ID for MQTT");
        return ESP_ERR_INVALID_STATE;
    }

    if (services_provisioning_load_device_name(s_device_name, sizeof(s_device_name)) != ESP_OK) {
        s_device_name[0] = '\0';
    }

    const char *broker_uri = config->broker_uri;
    const char *username = config->username;
    const char *password = config->password;

    ESP_LOGI(TAG, "Initializing MQTT client");
    ESP_LOGI(TAG, "Broker URI: %s", broker_uri);
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);
    if (s_device_name[0] != '\0') {
        ESP_LOGI(TAG, "Device Name: %s", s_device_name);
    }
    if (username != NULL) {
        ESP_LOGI(TAG, "Username: %s", username);
    }

    bool is_secure = (strncmp(broker_uri, "mqtts://", 8) == 0);
    if (is_secure && !services_provisioning_bridge_has_mqtt_ca()) {
        ESP_LOGE(TAG, "MQTTS requested but CA loader unavailable");
        return ESP_ERR_INVALID_STATE;
    }

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

    if (is_secure) {
        size_t ca_cert_len = 0;
        esp_err_t ret = mqtt_fetch_ca_certificate(s_mqtt_ca_cert, &ca_cert_len);
        if (ret == ESP_OK && ca_cert_len > 0) {
            mqtt_cfg.broker.verification.certificate = s_mqtt_ca_cert;
            mqtt_cfg.broker.verification.certificate_len = ca_cert_len + 1;
            ESP_LOGI(TAG, "Loaded CA certificate for MQTTS (%zu bytes)", ca_cert_len);
            mqtt_log_certificate_preview(ca_cert_len);
        } else {
            ESP_LOGW(TAG,
                     "CA certificate unavailable (%s), skipping verification",
                     esp_err_to_name(ret));
            mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        }
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_mqtt_client_register_event(client,
                                                    ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler,
                                                    NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(client);
        return ret;
    }

    ret = mqtt_connection_controller_set_client(client);
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(client);
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client initialized successfully");
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (mqtt_connection_controller_get_client() == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting MQTT client stack");

    if (s_publish_task_handle == NULL) {
        mqtt_publish_scheduler_init(&s_publish_scheduler, s_publish_interval_sec);
        mqtt_publish_loop_init(&s_publish_loop, &s_publish_scheduler, &s_publish_loop_config);

#ifdef CONFIG_FREERTOS_UNICORE
        BaseType_t task_ret = xTaskCreate(
            mqtt_publish_task,
            "mqtt_publish",
            4096,
            NULL,
            5,
            &s_publish_task_handle);
#else
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            mqtt_publish_task,
            "mqtt_publish",
            4096,
            NULL,
            5,
            &s_publish_task_handle,
            0);
#endif
        if (task_ret != pdPASS) {
            s_publish_task_handle = NULL;
            ESP_LOGE(TAG, "Failed to create MQTT publish task");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG,
                 "MQTT publish task started (interval=%lu seconds)",
                 (unsigned long)s_publish_interval_sec);
    }

    esp_err_t sup_ret = mqtt_connection_controller_start_supervisor();
    if (sup_ret != ESP_OK) {
        return sup_ret;
    }

    mqtt_connection_watchdog_start(NULL);
    mqtt_connection_controller_request_run(true);
    ESP_LOGI(TAG,
             "MQTT supervisor activated (state=%d)",
             mqtt_connection_controller_get_state());
    return ESP_OK;
}

esp_err_t mqtt_refresh_device_name(void)
{
    char new_device_name[SERVICES_DEVICE_NAME_MAX_LEN] = {0};
    esp_err_t err = services_provisioning_load_device_name(new_device_name,
                                                           sizeof(new_device_name));

    if (err == ESP_OK) {
        strncpy(s_device_name,
                new_device_name,
                sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';

        if (s_device_name[0] != '\0') {
            ESP_LOGI(TAG, "Device name updated: %s", s_device_name);
        } else {
            ESP_LOGI(TAG, "Device name cleared");
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "Failed to refresh device name: %s",
             esp_err_to_name(err));
    return ESP_FAIL;
}

esp_err_t mqtt_client_stop(void)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL) {
        return ESP_OK;
    }

    mqtt_connection_controller_request_run(false);
    mqtt_connection_watchdog_stop();

    if (s_publish_task_handle != NULL) {
        vTaskDelete(s_publish_task_handle);
        s_publish_task_handle = NULL;
    }

    if (mqtt_connection_controller_is_connected()) {
        mqtt_publish_status("offline");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Stopping MQTT client");
    return mqtt_connection_controller_stop_client();
}

esp_err_t mqtt_client_deinit(void)
{
    mqtt_client_stop();
    mqtt_connection_controller_stop_supervisor();

    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client != NULL) {
        ESP_LOGI(TAG, "Deinitializing MQTT client");
        esp_mqtt_client_destroy(client);
        mqtt_connection_controller_clear_client();
    }

    return ESP_OK;
}

bool mqtt_client_is_connected(void)
{
    return mqtt_connection_controller_is_connected();
}

mqtt_state_t mqtt_client_get_state(void)
{
    return mqtt_connection_controller_get_state();
}

esp_err_t mqtt_publish_telemetry(const telemetry_data_t *data)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL || !mqtt_connection_controller_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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

    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(root, "timestamp", tv.tv_sec);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/telemetry", s_device_id);

    int msg_id = esp_mqtt_client_publish(client, topic, json_str, 0, 1, 0);
    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish telemetry");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Telemetry published (msg_id=%d)", msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_kannacloud_data(const kannacloud_data_t *data)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL || !mqtt_connection_controller_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "device_id", data->device_id);
    if (s_device_name[0] != '\0') {
        cJSON_AddStringToObject(root, "device_name", s_device_name);
    }

    cJSON *sensors = cJSON_CreateObject();
    if (sensors == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    uint8_t sensor_count = sensor_manager_get_ezo_count();
    for (uint8_t i = 0; i < sensor_count; i++) {
        char sensor_type[16];
        float values[4];
        uint8_t value_count = 0;

        if (sensor_manager_read_ezo_sensor(i, sensor_type, values, &value_count) != ESP_OK) {
            continue;
        }

        if (value_count == 1) {
            cJSON_AddNumberToObject(sensors, sensor_type, values[0]);
            continue;
        }

        if (value_count > 1) {
            cJSON *sensor_obj = cJSON_CreateObject();
            if (sensor_obj == NULL) {
                continue;
            }

            if (strcmp(sensor_type, "HUM") == 0) {
                ezo_sensor_t *sensor = (ezo_sensor_t *)sensor_manager_get_ezo_sensor(i);
                if (sensor != NULL && sensor->config.hum.param_count > 0) {
                    for (uint8_t j = 0;
                         j < value_count && j < sensor->config.hum.param_count;
                         j++) {
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
                    if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "humidity", values[0]);
                    if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "air_temp", values[1]);
                    if (value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "dew_point", values[2]);
                }
            } else if (strcmp(sensor_type, "DO") == 0) {
                if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "dissolved_oxygen", values[0]);
                if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "saturation", values[1]);
            } else if (strcmp(sensor_type, "EC") == 0) {
                if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "conductivity", values[0]);
                if (value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "tds", values[1]);
                if (value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "salinity", values[2]);
                if (value_count >= 4) cJSON_AddNumberToObject(sensor_obj, "specific_gravity", values[3]);
            } else if (strcmp(sensor_type, "ORP") == 0) {
                if (value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "orp", values[0]);
            } else {
                for (uint8_t j = 0; j < value_count; j++) {
                    char field_name[16];
                    snprintf(field_name, sizeof(field_name), "value_%d", j);
                    cJSON_AddNumberToObject(sensor_obj, field_name, values[j]);
                }
            }

            cJSON_AddItemToObject(sensors, sensor_type, sensor_obj);
        }
    }

    cJSON_AddItemToObject(root, "sensors", sensors);

    if (!isnan(data->battery)) {
        cJSON_AddNumberToObject(root, "battery", data->battery);
    }
    cJSON_AddNumberToObject(root, "rssi", data->rssi);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "kannacloud/sensor/%s/data", data->device_id);

    int msg_id = esp_mqtt_client_publish(client, topic, json_str, 0, 1, 0);
    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish KannaCloud data");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "KannaCloud data published to %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char *status)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "status", status);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(root, "timestamp", tv.tv_sec);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/status", s_device_id);

    int msg_id = esp_mqtt_client_publish(client, topic, json_str, 0, 1, 1);
    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status published: %s (msg_id=%d)", status, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_json(const char *topic, const char *json_data, int qos, bool retain)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL || !mqtt_connection_controller_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || json_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_publish(client, topic, json_data, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published to %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_subscribe(const char *topic, int qos)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL || !mqtt_connection_controller_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Subscribed to %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_unsubscribe(const char *topic)
{
    esp_mqtt_client_handle_t client = mqtt_connection_controller_get_client();
    if (client == NULL || !mqtt_connection_controller_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_unsubscribe(client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to unsubscribe from %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Unsubscribed from %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_set_telemetry_interval(uint32_t interval_sec)
{
    uint32_t old_interval = s_publish_interval_sec;
    s_publish_interval_sec = interval_sec;
    mqtt_publish_scheduler_set_interval(&s_publish_scheduler, interval_sec);

    if (interval_sec == 0) {
        ESP_LOGI(TAG, "MQTT periodic publishing disabled");
    } else {
        ESP_LOGI(TAG,
                 "MQTT publish interval updated to %lu seconds",
                 (unsigned long)interval_sec);
        mqtt_publish_scheduler_request_publish(&s_publish_scheduler);
        if (old_interval == 0 && s_publish_task_handle != NULL) {
            ESP_LOGI(TAG, "Waking MQTT task to resume periodic publishing");
            xTaskNotifyGive(s_publish_task_handle);
        }
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, "mqtt_interval", interval_sec);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT interval %lu saved to NVS", (unsigned long)interval_sec);
    } else {
        ESP_LOGE(TAG, "Failed to persist MQTT interval: %s", esp_err_to_name(err));
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

    mqtt_publish_scheduler_request_publish(&s_publish_scheduler);
    xTaskNotifyGive(s_publish_task_handle);
    return ESP_OK;
}

void mqtt_telemetry_get_diagnostics(mqtt_telemetry_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    memset(out_diag, 0, sizeof(*out_diag));
    telemetry_pipeline_get_metrics(&s_telemetry_pipeline, &out_diag->pipeline_metrics);
    out_diag->publish_interval_sec = s_publish_interval_sec;
    out_diag->busy_backoff_ms = s_publish_loop_config.busy_backoff_ms;
    out_diag->client_backoff_ms = s_publish_loop_config.client_backoff_ms;
    out_diag->idle_delay_ms = s_publish_loop_config.idle_delay_ms;

    out_diag->interval_enabled = (s_publish_scheduler.interval_sec > 0);
    out_diag->manual_request_pending = s_publish_scheduler.manual_request;
    out_diag->publish_queue_depth = s_publish_scheduler.manual_request ? 1U : 0U;
    out_diag->scheduler_last_publish_us = s_publish_scheduler.last_publish_us;
    out_diag->scheduler_next_retry_us = s_publish_scheduler.next_retry_us;
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

esp_err_t mqtt_get_cached_sensor_data(kannacloud_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_cache_t cache = {0};
    esp_err_t err = sensor_manager_get_cached_data(&cache);
    if (err != ESP_OK) {
        return err;
    }

    memset(data, 0, sizeof(*data));
    strncpy(data->device_id, s_device_id, sizeof(data->device_id) - 1);
    data->device_id[sizeof(data->device_id) - 1] = '\0';
    data->battery = cache.battery_valid ? cache.battery_percentage : NAN;
    data->rssi = cache.rssi;

    mqtt_populate_sensor_data_from_cache(&cache, &data->sensors);
    return ESP_OK;
}
