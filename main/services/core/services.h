#pragma once

#include <stdbool.h>
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

typedef struct {
    bool enable_http_server;
    bool enable_mqtt;
    bool enable_mdns;
    bool enable_time_sync;
    bool tls_ready;
    bool time_synced;
    uint16_t https_port;
    uint32_t mqtt_publish_interval_sec;
    const char *mqtt_broker_uri;
    const char *mqtt_username;
    const char *mqtt_password;
    const char *mdns_hostname;
    const char *mdns_instance_name;
    const char *timezone;
    time_sync_callback_t time_sync_callback;
    uint32_t time_sync_timeout_sec;
} services_config_t;

typedef struct {
    EventGroupHandle_t boot_event_group;
    EventBits_t network_ready_bit;
    const sensor_pipeline_launch_ctx_t *sensor_pipeline_ctx;
} services_dependencies_t;

esp_err_t services_start(const services_config_t *config,
                         const services_dependencies_t *deps,
                         services_status_listener_t listener,
                         void *listener_ctx);

esp_err_t services_stop(void);

#ifdef __cplusplus
}
#endif
