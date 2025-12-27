#include "services/core/services.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "mdns_service.h"
#include "services/http/http_server.h"
#include "services/telemetry/mqtt_telemetry.h"

typedef struct {
    bool running;
    bool http_started;
    bool mdns_started;
    bool mqtt_initialized;
    bool mqtt_started;
    services_config_t config;
    services_dependencies_t deps;
    services_status_listener_t listener;
    void *listener_ctx;
} services_state_t;

static const char *TAG = "SERVICES_CORE";
static services_state_t s_state = {0};

static void services_emit_event(services_event_type_t type,
                                esp_err_t result,
                                const char *component)
{
    if (s_state.listener == NULL) {
        return;
    }

    services_status_event_t event = {
        .type = type,
        .component = component,
        .result = result,
    };
    s_state.listener(&event, s_state.listener_ctx);
}

static void services_set_ready_bit(void)
{
    if (s_state.deps.boot_event_group == NULL || s_state.deps.network_ready_bit == 0) {
        return;
    }

    xEventGroupSetBits(s_state.deps.boot_event_group, s_state.deps.network_ready_bit);
}

static void services_start_mdns(void)
{
    if (!s_state.config.enable_mdns) {
        return;
    }

    if (!s_state.config.tls_ready) {
        ESP_LOGW(TAG, "TLS assets unavailable; skipping mDNS service");
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
    if (!s_state.config.tls_ready) {
        ESP_LOGW(TAG, "TLS assets unavailable; skipping HTTPS server startup");
        return;
    }

    if (!s_state.config.time_synced) {
        ESP_LOGW(TAG, "Time sync incomplete; HTTPS clients may warn about certificates");
    }

    esp_err_t ret = http_server_start();
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

    esp_err_t ret = mqtt_client_init(s_state.config.mqtt_broker_uri,
                                     s_state.config.mqtt_username,
                                     s_state.config.mqtt_password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(ret));
        services_emit_event(SERVICES_EVENT_DEGRADED, ret, "mqtt");
        return;
    }

    s_state.mqtt_initialized = true;

    if (s_state.config.mqtt_publish_interval_sec > 0) {
        mqtt_set_telemetry_interval(s_state.config.mqtt_publish_interval_sec);
    }

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

    ESP_LOGI(TAG, "Services core initializing (HTTP=%d, MQTT=%d, mDNS=%d, time_sync=%d)",
             config->enable_http_server,
             config->enable_mqtt,
             config->enable_mdns,
             config->enable_time_sync);

    services_emit_event(SERVICES_EVENT_STARTING, ESP_OK, NULL);

    services_start_mdns();
    services_start_http();
    services_start_mqtt();

    services_set_ready_bit();

    const bool any_enabled = config->enable_http_server || config->enable_mdns ||
                             config->enable_mqtt || config->enable_time_sync;
    const bool any_started = s_state.http_started || s_state.mdns_started ||
                             s_state.mqtt_started;

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

    s_state.running = false;
    services_emit_event(SERVICES_EVENT_STOPPED, ESP_OK, NULL);
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "Services core stopped");
    return ESP_OK;
}
