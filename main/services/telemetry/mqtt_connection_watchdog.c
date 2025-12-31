#include "services/telemetry/mqtt_connection_watchdog.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "services/core/services.h"
#include "services/telemetry/mqtt_connection_controller.h"

#define MQTT_WATCHDOG_DEFAULT_POLL_MS       5000U
#define MQTT_WATCHDOG_DEFAULT_STUCK_MS     60000U
#define MQTT_WATCHDOG_DEFAULT_REPORT_MS    30000U

static const char *TAG = "MQTT_WATCHDOG";

typedef struct {
    uint32_t poll_interval_ms;
    uint32_t stuck_threshold_ms;
    uint32_t report_interval_ms;
} mqtt_watchdog_state_t;

static esp_timer_handle_t s_watchdog_timer = NULL;
static mqtt_watchdog_state_t s_watchdog_state = {0};
static int64_t s_unhealthy_since_us = 0;
static int64_t s_last_report_us = 0;
static bool s_watchdog_tripped = false;
static uint32_t s_trip_count = 0;

static void mqtt_watchdog_reset(void)
{
    s_unhealthy_since_us = 0;
    s_watchdog_tripped = false;
}

static bool mqtt_watchdog_should_monitor(const mqtt_connection_metrics_t *metrics)
{
    if (metrics == NULL) {
        return false;
    }

    if (!metrics->should_run) {
        return false;
    }

    if (!metrics->supervisor_running) {
        return true;
    }

    return metrics->state != MQTT_STATE_CONNECTED;
}

static void mqtt_watchdog_timer_cb(void *arg)
{
    (void)arg;

    mqtt_connection_metrics_t metrics = {0};
    mqtt_connection_controller_get_metrics(&metrics);

    const int64_t now_us = esp_timer_get_time();

    if (!mqtt_watchdog_should_monitor(&metrics)) {
        mqtt_watchdog_reset();
        return;
    }

    if (metrics.state == MQTT_STATE_CONNECTED) {
        mqtt_watchdog_reset();
        return;
    }

    if (s_unhealthy_since_us == 0) {
        s_unhealthy_since_us = now_us;
        return;
    }

    int64_t elapsed_ms = (now_us - s_unhealthy_since_us) / 1000;
    if (elapsed_ms < (int64_t)s_watchdog_state.stuck_threshold_ms) {
        return;
    }

    bool can_report = !s_watchdog_tripped;
    if (!can_report) {
        int64_t since_last_report_ms = (now_us - s_last_report_us) / 1000;
        can_report = since_last_report_ms >= (int64_t)s_watchdog_state.report_interval_ms;
    }

    if (can_report) {
        ESP_LOGW(TAG,
                 "MQTT watchdog tripped: state=%d should_run=%d supervisor=%d elapsed=%lldms",
                 metrics.state,
                 (int)metrics.should_run,
                 (int)metrics.supervisor_running,
                 (long long)elapsed_ms);
        services_report_degraded("mqtt", ESP_ERR_TIMEOUT);
        s_watchdog_tripped = true;
        s_last_report_us = now_us;
        s_trip_count++;
    }
}

static void mqtt_watchdog_load_config(const mqtt_connection_watchdog_config_t *config)
{
    s_watchdog_state.poll_interval_ms = (config != NULL && config->poll_interval_ms > 0)
                                            ? config->poll_interval_ms
                                            : MQTT_WATCHDOG_DEFAULT_POLL_MS;
    s_watchdog_state.stuck_threshold_ms = (config != NULL && config->stuck_threshold_ms > 0)
                                              ? config->stuck_threshold_ms
                                              : MQTT_WATCHDOG_DEFAULT_STUCK_MS;
    s_watchdog_state.report_interval_ms = (config != NULL && config->report_interval_ms > 0)
                                              ? config->report_interval_ms
                                              : MQTT_WATCHDOG_DEFAULT_REPORT_MS;
}

esp_err_t mqtt_connection_watchdog_start(const mqtt_connection_watchdog_config_t *config)
{
    if (s_watchdog_timer != NULL) {
        return ESP_OK;
    }

    mqtt_watchdog_load_config(config);
    s_trip_count = 0;
    s_last_report_us = 0;

    const esp_timer_create_args_t args = {
        .callback = mqtt_watchdog_timer_cb,
        .arg = NULL,
        .name = "mqtt_wd",
        .dispatch_method = ESP_TIMER_TASK,
    };

    esp_err_t ret = esp_timer_create(&args, &s_watchdog_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create watchdog timer: %s", esp_err_to_name(ret));
        s_watchdog_timer = NULL;
        return ret;
    }

    ret = esp_timer_start_periodic(s_watchdog_timer, s_watchdog_state.poll_interval_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start watchdog timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_watchdog_timer);
        s_watchdog_timer = NULL;
        return ret;
    }

    ESP_LOGI(TAG,
             "Watchdog armed (poll=%ums stuck=%ums report=%ums)",
             (unsigned)s_watchdog_state.poll_interval_ms,
             (unsigned)s_watchdog_state.stuck_threshold_ms,
             (unsigned)s_watchdog_state.report_interval_ms);
    mqtt_watchdog_reset();
    return ESP_OK;
}

void mqtt_connection_watchdog_stop(void)
{
    if (s_watchdog_timer != NULL) {
        esp_timer_stop(s_watchdog_timer);
        esp_timer_delete(s_watchdog_timer);
        s_watchdog_timer = NULL;
    }

    mqtt_watchdog_reset();
}

void mqtt_connection_watchdog_get_metrics(mqtt_watchdog_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    metrics->active = (s_watchdog_timer != NULL);
    metrics->tripped = s_watchdog_tripped;
    metrics->trip_count = s_trip_count;
    metrics->unhealthy_since_us = s_unhealthy_since_us;
    metrics->last_report_us = s_last_report_us;
    metrics->poll_interval_ms = s_watchdog_state.poll_interval_ms;
    metrics->stuck_threshold_ms = s_watchdog_state.stuck_threshold_ms;
    metrics->report_interval_ms = s_watchdog_state.report_interval_ms;
}
