#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sensors/pipeline.h"
#include "services/time_sync/time_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICES_EVENT_STARTING = 0,
    SERVICES_EVENT_READY,
    SERVICES_EVENT_DEGRADED,
    SERVICES_EVENT_STOPPED,
} services_event_type_t;

typedef struct {
    services_event_type_t type;
    const char *component;  // optional, NULL means "core"
    esp_err_t result;
} services_status_event_t;

typedef void (*services_status_listener_t)(const services_status_event_t *event, void *user_ctx);
typedef void (*services_fault_handler_t)(const services_status_event_t *event, void *user_ctx);

#define SERVICES_TLS_CERT_MAX_BYTES   (4096U)
#define SERVICES_TLS_KEY_MAX_BYTES    (4096U)
#define SERVICES_MQTT_CA_MAX_BYTES    (4096U)
#define SERVICES_DEVICE_ID_MAX_LEN    (32U)
#define SERVICES_DEVICE_NAME_MAX_LEN  (65U)

typedef esp_err_t (*services_blob_loader_t)(void *ctx, char *buffer, size_t *length);
typedef esp_err_t (*services_string_loader_t)(void *ctx, char *buffer, size_t length);
typedef esp_err_t (*services_string_setter_t)(void *ctx, const char *value);
typedef esp_err_t (*services_void_action_t)(void *ctx);

typedef struct services_provisioning_api services_provisioning_api_t;

struct services_provisioning_api {
    services_blob_loader_t load_https_certificate;
    services_blob_loader_t load_https_private_key;
    services_blob_loader_t load_mqtt_ca_certificate;
    services_string_loader_t load_device_id;
    services_string_loader_t load_device_name;
    services_string_setter_t set_device_name;
    services_void_action_t clear_device_name;
    void *ctx;
};

typedef struct {
    bool enable_http_server;
    bool enable_mqtt;
    bool enable_mdns;
    bool enable_time_sync;
    bool tls_ready;
    bool time_synced;
    uint16_t https_port;
    uint32_t mqtt_publish_interval_sec;
    uint32_t telemetry_busy_backoff_ms;
    uint32_t telemetry_client_backoff_ms;
    uint32_t telemetry_idle_delay_ms;
    const char *mqtt_broker_uri;
    const char *mqtt_username;
    const char *mqtt_password;
    const char *mdns_hostname;
    const char *mdns_instance_name;
    const char *timezone;
    time_sync_callback_t time_sync_callback;
    uint32_t time_sync_timeout_sec;
    uint32_t time_sync_retry_delay_sec;
    uint8_t time_sync_retry_attempts;
    const services_provisioning_api_t *provisioning;
} services_config_t;

typedef struct {
    EventGroupHandle_t boot_event_group;
    EventBits_t network_ready_bit;
    EventBits_t degraded_bit;
    const sensor_pipeline_launch_ctx_t *sensor_pipeline_ctx;
    services_fault_handler_t fault_handler;
    void *fault_handler_ctx;
} services_dependencies_t;

esp_err_t services_start(const services_config_t *config,
                         const services_dependencies_t *deps,
                         services_status_listener_t listener,
                         void *listener_ctx);

esp_err_t services_stop(void);

void services_report_degraded(const char *component, esp_err_t result);

#ifdef __cplusplus
}
#endif
