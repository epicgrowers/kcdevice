#include "services/core/services.h"
#include "services/core/services_provisioning_bridge.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/discovery/mdns_service.h"
#include "services/http/http_server.h"
#include "services/telemetry/mqtt_connection_controller.h"
#include "services/telemetry/mqtt_telemetry.h"
#include "services/time_sync/time_sync.h"

typedef struct {
    bool running;
    bool http_started;
    bool mdns_started;
    bool mqtt_initialized;
    bool mqtt_started;
    bool time_sync_started;
    bool time_sync_ready;
    services_config_t config;
    services_dependencies_t deps;
    services_status_listener_t listener;
    void *listener_ctx;
} services_state_t;

static const char *TAG = "SERVICES_CORE";
static services_state_t s_state = {0};

static void services_set_ready_bit(void);
static void services_set_degraded_bit(void);
static void services_handle_fault(const services_status_event_t *event);
static bool services_wait_for_time_sync(uint32_t timeout_sec);
static bool services_time_sync_required_for_tls(void);
static bool services_ensure_time_sync_ready(const char *component);
static void services_start_time_sync(void);

static void services_emit_event(services_event_type_t type,
                                esp_err_t result,
                                const char *component)
{
    services_status_event_t event = {
        .type = type,
        .component = component,
        .result = result,
    };

    if (s_state.listener != NULL) {
        s_state.listener(&event, s_state.listener_ctx);
    }

    services_handle_fault(&event);
}

void services_report_degraded(const char *component, esp_err_t result)
{
    services_emit_event(SERVICES_EVENT_DEGRADED, result, component);
}

static void services_set_ready_bit(void)
{
    if (s_state.deps.boot_event_group == NULL || s_state.deps.network_ready_bit == 0) {
        return;
    }

    xEventGroupSetBits(s_state.deps.boot_event_group, s_state.deps.network_ready_bit);
}

static void services_set_degraded_bit(void)
{
    if (s_state.deps.boot_event_group == NULL || s_state.deps.degraded_bit == 0) {
        return;
    }

    xEventGroupSetBits(s_state.deps.boot_event_group, s_state.deps.degraded_bit);
}

esp_err_t services_handle_network_recovered(void)
{
    if (!s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Network recovery detected; evaluating dependent services");

    if (s_state.config.enable_time_sync) {
        bool needs_sync = !time_sync_is_synced() || !s_state.time_sync_ready;
        if (needs_sync) {
            if (s_state.time_sync_started) {
                time_sync_deinit();
                s_state.time_sync_started = false;
                s_state.time_sync_ready = false;
            }
            services_start_time_sync();
        }
    }

    if (s_state.config.enable_mqtt && s_state.mqtt_initialized) {
        mqtt_connection_controller_request_run(true);
    }

    services_emit_event(SERVICES_EVENT_READY, ESP_OK, "network_recovered");
    return ESP_OK;
}

static void services_handle_fault(const services_status_event_t *event)
{
    if (event == NULL || event->type != SERVICES_EVENT_DEGRADED) {
        return;
    }

    services_set_degraded_bit();

    if (s_state.deps.fault_handler != NULL) {
        s_state.deps.fault_handler(event, s_state.deps.fault_handler_ctx);
    }
}

static bool services_wait_for_time_sync(uint32_t timeout_sec)
{
    uint32_t effective_timeout = (timeout_sec > 0) ? timeout_sec : 10;

    for (uint32_t waited = 0; waited < effective_timeout; waited++) {
        if (time_sync_is_synced()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return time_sync_is_synced();
}

static bool services_time_sync_required_for_tls(void)
{
    if (!s_state.config.require_time_sync_for_tls) {
        return false;
    }

    if (!s_state.config.enable_time_sync) {
        return false;
    }

    if (s_state.time_sync_ready || s_state.config.time_synced) {
        return false;
    }

    return true;
}

static bool services_ensure_time_sync_ready(const char *component)
{
    if (!services_time_sync_required_for_tls()) {
        return true;
    }

    ESP_LOGW(TAG,
             "%s startup blocked: time sync not ready",
             (component != NULL) ? component : "unknown");
    services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, component);
    return false;
}

static void services_start_time_sync(void)
{
    if (!s_state.config.enable_time_sync) {
        return;
    }

    const char *timezone = s_state.config.timezone;
    if (timezone == NULL || timezone[0] == '\0') {
        timezone = "UTC";
    }

    uint32_t timeout_sec = (s_state.config.time_sync_timeout_sec > 0)
                               ? s_state.config.time_sync_timeout_sec
                               : 10;
    uint32_t retry_delay_sec = (s_state.config.time_sync_retry_delay_sec > 0)
                                   ? s_state.config.time_sync_retry_delay_sec
                                   : 5;
    uint32_t total_attempts = (uint32_t)s_state.config.time_sync_retry_attempts + 1U;

    bool synced = false;
    bool start_event_sent = false;

    for (uint32_t attempt = 0; attempt < total_attempts; attempt++) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Time sync retry %u/%u (timeout=%us)",
                     (unsigned)(attempt + 1U), (unsigned)total_attempts, (unsigned)timeout_sec);
        }

        esp_err_t ret = time_sync_init(timezone, s_state.config.time_sync_callback);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Time sync init failed: %s", esp_err_to_name(ret));
            services_emit_event(SERVICES_EVENT_DEGRADED, ret, "time_sync");
            return;
        }

        s_state.time_sync_started = true;

        if (!start_event_sent) {
            services_emit_event(SERVICES_EVENT_STARTING, ESP_OK, "time_sync");
            start_event_sent = true;
        }

        if (services_wait_for_time_sync(timeout_sec)) {
            s_state.time_sync_ready = true;
            s_state.config.time_synced = true;
            services_emit_event(SERVICES_EVENT_READY, ESP_OK, "time_sync");
            synced = true;
            break;
        }

        ESP_LOGW(TAG, "Time sync attempt %u/%u timed out",
                 (unsigned)(attempt + 1U), (unsigned)total_attempts);

        time_sync_deinit();
        s_state.time_sync_started = false;

        if (attempt + 1U < total_attempts) {
            ESP_LOGI(TAG, "Retrying SNTP in %u seconds", (unsigned)retry_delay_sec);
            if (retry_delay_sec > 0) {
                vTaskDelay(pdMS_TO_TICKS(retry_delay_sec * 1000U));
            }
        }
    }

    if (!synced) {
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_TIMEOUT, "time_sync");
        if (s_state.config.time_sync_callback != NULL) {
            s_state.config.time_sync_callback(false, NULL);
        }
    }
}

static void services_start_mdns(void)
{
    if (!s_state.config.enable_mdns) {
        return;
    }

    if (!services_ensure_time_sync_ready("mdns")) {
        return;
    }

    if (!s_state.config.tls_ready) {
        ESP_LOGW(TAG, "TLS assets unavailable; skipping mDNS service");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "mdns");
        return;
    }

    const char *hostname = (s_state.config.mdns_hostname != NULL)
                               ? s_state.config.mdns_hostname
                               : "kc";
    const char *instance_name = (s_state.config.mdns_instance_name != NULL)
                                    ? s_state.config.mdns_instance_name
                                    : "KannaCloud Device";

    esp_err_t ret = mdns_service_init(hostname, instance_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        services_emit_event(SERVICES_EVENT_DEGRADED, ret, "mdns");
        return;
    }

    ret = mdns_service_add_https(s_state.config.https_port);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to advertise HTTPS over mDNS: %s", esp_err_to_name(ret));
    }

    s_state.mdns_started = true;
    services_emit_event(SERVICES_EVENT_READY, ESP_OK, "mdns");
}

static void services_start_http(void)
{
    if (!s_state.config.enable_http_server) {
        return;
    }

#ifdef CONFIG_IDF_TARGET_ESP32C6
    ESP_LOGW(TAG, "HTTPS dashboard disabled on ESP32-C6 target");
    services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_NOT_SUPPORTED, "http");
    return;
#else
    if (!services_ensure_time_sync_ready("http")) {
        return;
    }

    if (!s_state.config.tls_ready) {
        ESP_LOGW(TAG, "TLS assets unavailable; skipping HTTPS server startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "http");
        return;
    }

    if (!services_provisioning_bridge_has_https_credentials()) {
        ESP_LOGW(TAG, "Provisioning hooks unavailable; skipping HTTPS server startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "http");
        return;
    }

    if (!s_state.time_sync_ready && !s_state.config.time_synced) {
        ESP_LOGW(TAG, "Time sync incomplete; HTTPS clients may warn about certificates");
    }

    http_server_config_t http_cfg = {
        .https_port = s_state.config.https_port,
        .time_synced = s_state.time_sync_ready || s_state.config.time_synced,
    };

    esp_err_t ret = http_server_start(&http_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server start failed: %s", esp_err_to_name(ret));
        services_emit_event(SERVICES_EVENT_DEGRADED, ret, "http");
        return;
    }

    s_state.http_started = true;
    ESP_LOGI(TAG, "HTTPS dashboard server ready on port %u", (unsigned)s_state.config.https_port);
    services_emit_event(SERVICES_EVENT_READY, ESP_OK, "http");
#endif
}

static void services_start_mqtt(void)
{
    if (!s_state.config.enable_mqtt) {
        return;
    }

    if (!services_ensure_time_sync_ready("mqtt")) {
        return;
    }

    if (!s_state.config.tls_ready) {
        ESP_LOGW(TAG, "TLS assets unavailable; skipping MQTT telemetry startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "mqtt");
        return;
    }

    if (s_state.config.mqtt_broker_uri == NULL || s_state.config.mqtt_broker_uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT broker URI missing; skipping telemetry startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_ARG, "mqtt");
        return;
    }

    if (!services_provisioning_bridge_has_device_identity()) {
        ESP_LOGW(TAG, "Provisioning hooks unavailable; skipping MQTT telemetry startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "mqtt");
        return;
    }

    if (!services_provisioning_bridge_has_mqtt_ca()) {
        ESP_LOGW(TAG, "MQTT CA certificate loader unavailable; skipping MQTT telemetry startup");
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_ERR_INVALID_STATE, "mqtt");
        return;
    }

    mqtt_telemetry_config_t mqtt_cfg = {
        .broker_uri = s_state.config.mqtt_broker_uri,
        .username = s_state.config.mqtt_username,
        .password = s_state.config.mqtt_password,
        .publish_interval_sec = s_state.config.mqtt_publish_interval_sec,
        .busy_backoff_ms = s_state.config.telemetry_busy_backoff_ms,
        .client_backoff_ms = s_state.config.telemetry_client_backoff_ms,
        .idle_delay_ms = s_state.config.telemetry_idle_delay_ms,
        .sensor_ctx = s_state.deps.sensor_pipeline_ctx,
    };

    esp_err_t ret = mqtt_client_init(&mqtt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(ret));
        services_emit_event(SERVICES_EVENT_DEGRADED, ret, "mqtt");
        return;
    }

    s_state.mqtt_initialized = true;

    ret = mqtt_client_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(ret));
        mqtt_client_deinit();
        s_state.mqtt_initialized = false;
        services_emit_event(SERVICES_EVENT_DEGRADED, ret, "mqtt");
        return;
    }

    s_state.mqtt_started = true;
    ESP_LOGI(TAG, "MQTT telemetry client started (broker=%s)", s_state.config.mqtt_broker_uri);
    services_emit_event(SERVICES_EVENT_READY, ESP_OK, "mqtt");
}

esp_err_t services_start(const services_config_t *config,
                         const services_dependencies_t *deps,
                         services_status_listener_t listener,
                         void *listener_ctx)
{
    if (config == NULL || deps == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.running) {
        ESP_LOGW(TAG, "services_start() called while already running");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.config = *config;
    s_state.deps = *deps;
    s_state.listener = listener;
    s_state.listener_ctx = listener_ctx;
    s_state.running = true;

    services_provisioning_bridge_init(s_state.config.provisioning);

    ESP_LOGI(TAG, "Services core initializing (HTTP=%d, MQTT=%d, mDNS=%d, time_sync=%d)",
             config->enable_http_server,
             config->enable_mqtt,
             config->enable_mdns,
             config->enable_time_sync);

    services_emit_event(SERVICES_EVENT_STARTING, ESP_OK, NULL);

    services_start_time_sync();
    services_start_mdns();
    services_start_http();
    services_start_mqtt();

    services_set_ready_bit();

    const bool any_enabled = config->enable_http_server || config->enable_mdns ||
                             config->enable_mqtt || config->enable_time_sync;
    const bool any_started = s_state.http_started || s_state.mdns_started ||
                             s_state.mqtt_started || s_state.time_sync_started;

    if (!any_enabled || any_started) {
        services_emit_event(SERVICES_EVENT_READY, ESP_OK, NULL);
    } else {
        services_emit_event(SERVICES_EVENT_DEGRADED, ESP_FAIL, NULL);
    }

    return ESP_OK;
}

esp_err_t services_stop(void)
{
    if (!s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

#ifndef CONFIG_IDF_TARGET_ESP32C6
    if (s_state.http_started) {
        http_server_stop();
        s_state.http_started = false;
        services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, "http");
    }
#endif

    if (s_state.mdns_started) {
        mdns_service_deinit();
        s_state.mdns_started = false;
        services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, "mdns");
    }

    if (s_state.mqtt_started) {
        mqtt_client_stop();
        s_state.mqtt_started = false;
    }

    if (s_state.mqtt_initialized) {
        mqtt_client_deinit();
        s_state.mqtt_initialized = false;
        services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, "mqtt");
    }

    if (s_state.time_sync_started) {
        time_sync_deinit();
        s_state.time_sync_started = false;
        s_state.time_sync_ready = false;
        services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, "time_sync");
    }

    if (s_state.deps.boot_event_group != NULL && s_state.deps.degraded_bit != 0) {
        xEventGroupClearBits(s_state.deps.boot_event_group, s_state.deps.degraded_bit);
    }

    services_provisioning_bridge_deinit();

    s_state.running = false;
    services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, NULL);
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "Services core stopped");
    return ESP_OK;
}
