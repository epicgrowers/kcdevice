#include "services/core/services_config.h"

static const char s_default_mqtt_broker_uri[] = "mqtts://mqtt.kannacloud.com:8883";
static const char s_default_mqtt_username[] = "sensor01";
static const char s_default_mqtt_password[] = "xkKKYQWxiT83Ni3";
static const char s_default_mdns_hostname[] = "kc";
static const char s_default_mdns_instance[] = "KannaCloud Device";

static const services_config_t s_services_defaults = {
    .enable_http_server = false,
    .enable_mqtt = false,
    .enable_mdns = false,
    .enable_time_sync = true,
    .require_time_sync_for_tls = true,
    .tls_ready = false,
    .time_synced = false,
    .https_port = 443,
    .mqtt_publish_interval_sec = 0,
    .telemetry_busy_backoff_ms = 1000,
    .telemetry_client_backoff_ms = 750,
    .telemetry_idle_delay_ms = 200,
    .mqtt_broker_uri = s_default_mqtt_broker_uri,
    .mqtt_username = s_default_mqtt_username,
    .mqtt_password = s_default_mqtt_password,
    .mdns_hostname = s_default_mdns_hostname,
    .mdns_instance_name = s_default_mdns_instance,
    .timezone = NULL,
    .time_sync_callback = NULL,
    .time_sync_timeout_sec = 30,
    .time_sync_retry_delay_sec = 15,
    .time_sync_retry_attempts = 2,
    .provisioning = NULL,
};

void services_config_load_defaults(services_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = s_services_defaults;
}
