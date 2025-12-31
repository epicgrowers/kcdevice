#include "services/telemetry/mqtt_connection_controller.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MQTT_RETRY_DELAY_MS 5000

static const char *TAG = "MQTT_CONN";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static TaskHandle_t s_mqtt_supervisor_task = NULL;
static bool s_mqtt_should_run = false;
static bool s_mqtt_client_running = false;
static mqtt_state_t s_mqtt_state = MQTT_STATE_DISCONNECTED;
static uint32_t s_mqtt_reconnects = 0;
static uint32_t s_consecutive_failures = 0;
static int64_t s_last_transition_us = 0;

static void mqtt_connection_controller_mark_connecting(void);
static void mqtt_connection_controller_update_transition(mqtt_state_t new_state);
static void mqtt_connection_controller_note_failure(void);

static void mqtt_supervisor_task(void *arg)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(500);
    const TickType_t retry_delay = pdMS_TO_TICKS(MQTT_RETRY_DELAY_MS);

    ESP_LOGI(TAG, "MQTT supervisor task started");

    while (1) {
        if (s_mqtt_client == NULL) {
            break;
        }

        if (!s_mqtt_should_run) {
            vTaskDelay(poll_delay);
            continue;
        }

        if (s_mqtt_client_running || s_mqtt_state == MQTT_STATE_CONNECTING) {
            vTaskDelay(poll_delay);
            continue;
        }

        ESP_LOGI(TAG, "MQTT supervisor: attempting client start (state=%d)", s_mqtt_state);
        esp_err_t ret = esp_mqtt_client_start(s_mqtt_client);
        if (ret == ESP_OK) {
            mqtt_connection_controller_mark_connecting();
            s_mqtt_client_running = true;
            continue;
        }

        ESP_LOGW(TAG, "MQTT supervisor: start failed (%s). Retrying in %d ms",
                 esp_err_to_name(ret), MQTT_RETRY_DELAY_MS);
        mqtt_connection_controller_handle_error();
        vTaskDelay(retry_delay);
    }

    ESP_LOGI(TAG, "MQTT supervisor exiting");
    s_mqtt_supervisor_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mqtt_connection_controller_set_client(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mqtt_client = client;
    mqtt_connection_controller_update_transition(MQTT_STATE_DISCONNECTED);
    s_mqtt_should_run = false;
    s_mqtt_client_running = false;
    s_mqtt_reconnects = 0;
    s_consecutive_failures = 0;
    return ESP_OK;
}

void mqtt_connection_controller_clear_client(void)
{
    s_mqtt_client = NULL;
    s_mqtt_should_run = false;
    s_mqtt_client_running = false;
    mqtt_connection_controller_update_transition(MQTT_STATE_DISCONNECTED);
    s_mqtt_reconnects = 0;
    s_consecutive_failures = 0;
}

esp_mqtt_client_handle_t mqtt_connection_controller_get_client(void)
{
    return s_mqtt_client;
}

esp_err_t mqtt_connection_controller_start_supervisor(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mqtt_supervisor_task != NULL) {
        return ESP_OK;
    }

#ifdef CONFIG_FREERTOS_UNICORE
    BaseType_t sup_ret = xTaskCreate(
        mqtt_supervisor_task,
        "mqtt_supervisor",
        3072,
        NULL,
        4,
        &s_mqtt_supervisor_task);
#else
    BaseType_t sup_ret = xTaskCreatePinnedToCore(
        mqtt_supervisor_task,
        "mqtt_supervisor",
        3072,
        NULL,
        4,
        &s_mqtt_supervisor_task,
        0);
#endif

    if (sup_ret != pdPASS) {
        s_mqtt_supervisor_task = NULL;
        ESP_LOGE(TAG, "Failed to create MQTT supervisor task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_connection_controller_stop_supervisor(void)
{
    if (s_mqtt_supervisor_task != NULL) {
        TaskHandle_t handle = s_mqtt_supervisor_task;
        s_mqtt_supervisor_task = NULL;
        vTaskDelete(handle);
    }

    return ESP_OK;
}

void mqtt_connection_controller_request_run(bool should_run)
{
    s_mqtt_should_run = should_run;
}

esp_err_t mqtt_connection_controller_stop_client(void)
{
    if (s_mqtt_client == NULL || !s_mqtt_client_running) {
        s_mqtt_client_running = false;
        mqtt_connection_controller_update_transition(MQTT_STATE_DISCONNECTED);
        return ESP_OK;
    }

    esp_err_t ret = esp_mqtt_client_stop(s_mqtt_client);
    if (ret == ESP_OK) {
        s_mqtt_client_running = false;
        mqtt_connection_controller_update_transition(MQTT_STATE_DISCONNECTED);
    }

    return ret;
}

mqtt_state_t mqtt_connection_controller_get_state(void)
{
    return s_mqtt_state;
}

bool mqtt_connection_controller_is_connected(void)
{
    return s_mqtt_state == MQTT_STATE_CONNECTED;
}

bool mqtt_connection_controller_is_client_running(void)
{
    return s_mqtt_client_running;
}

uint32_t mqtt_connection_controller_get_reconnects(void)
{
    return s_mqtt_reconnects;
}

void mqtt_connection_controller_get_metrics(mqtt_connection_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    int64_t transition_us = s_last_transition_us;
    if (transition_us == 0) {
        transition_us = esp_timer_get_time();
    }

    metrics->state = s_mqtt_state;
    metrics->should_run = s_mqtt_should_run;
    metrics->client_running = s_mqtt_client_running;
    metrics->supervisor_running = (s_mqtt_supervisor_task != NULL);
    metrics->reconnect_count = s_mqtt_reconnects;
    metrics->consecutive_failures = s_consecutive_failures;
    metrics->last_transition_us = transition_us;
}

static void mqtt_connection_controller_update_transition(mqtt_state_t new_state)
{
    s_mqtt_state = new_state;
    s_last_transition_us = esp_timer_get_time();
}

static void mqtt_connection_controller_note_failure(void)
{
    if (s_mqtt_should_run) {
        s_consecutive_failures++;
    } else {
        s_consecutive_failures = 0;
    }
}

static void mqtt_connection_controller_mark_connecting(void)
{
    mqtt_connection_controller_update_transition(MQTT_STATE_CONNECTING);
}

void mqtt_connection_controller_handle_connected(void)
{
    mqtt_connection_controller_update_transition(MQTT_STATE_CONNECTED);
    s_consecutive_failures = 0;
}

void mqtt_connection_controller_handle_disconnected(void)
{
    mqtt_connection_controller_update_transition(MQTT_STATE_DISCONNECTED);
    s_mqtt_client_running = false;
    s_mqtt_reconnects++;
    mqtt_connection_controller_note_failure();
}

void mqtt_connection_controller_handle_error(void)
{
    mqtt_connection_controller_update_transition(MQTT_STATE_ERROR);
    s_mqtt_client_running = false;
    mqtt_connection_controller_note_failure();
}
