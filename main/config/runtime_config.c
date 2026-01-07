#include "config/runtime_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "runtime_config_hash.h"

extern const uint8_t config_runtime_services_json_start[] asm("_binary_services_json_start");
extern const uint8_t config_runtime_services_json_end[] asm("_binary_services_json_end");

static void runtime_copy_string(char *dest, size_t dest_size, const char *src);

typedef struct {
    bool parsed;
    esp_err_t status;
    bool enable_http_server_present;
    bool enable_http_server;
    bool enable_mqtt_present;
    bool enable_mqtt;
    bool enable_mdns_present;
    bool enable_mdns;
    bool enable_time_sync_present;
    bool enable_time_sync;
    bool require_time_sync_for_tls_present;
    bool require_time_sync_for_tls;
    bool https_port_present;
    uint16_t https_port;
    bool mqtt_interval_present;
    uint32_t mqtt_interval_sec;
    bool telemetry_interval_present;
    uint32_t telemetry_publish_interval_sec;
    bool telemetry_busy_backoff_present;
    uint32_t telemetry_busy_backoff_ms;
    bool telemetry_client_backoff_present;
    uint32_t telemetry_client_backoff_ms;
    bool telemetry_idle_delay_present;
    uint32_t telemetry_idle_delay_ms;
    bool time_sync_timeout_present;
    uint32_t time_sync_timeout_sec;
    bool time_sync_retry_attempts_present;
    uint32_t time_sync_retry_attempts;
    bool time_sync_retry_delay_present;
    uint32_t time_sync_retry_delay_sec;
    bool mqtt_broker_uri_present;
    char mqtt_broker_uri[128];
    bool mqtt_username_present;
    char mqtt_username[64];
    bool mqtt_password_present;
    char mqtt_password[64];
    bool mdns_hostname_present;
    char mdns_hostname[32];
    bool mdns_instance_present;
    char mdns_instance[64];
    bool timezone_present;
    char timezone[32];
} runtime_services_overrides_t;

static const char *TAG = "RUNTIME_CFG";
static runtime_services_overrides_t s_overrides = {0};
static const runtime_config_digest_t s_runtime_digests = {
    .services_sha256 = RUNTIME_CONFIG_SERVICES_SHA256,
    .api_keys_sha256 = RUNTIME_CONFIG_API_KEYS_SHA256,
    .services_size_bytes = RUNTIME_CONFIG_SERVICES_SIZE_BYTES,
    .api_keys_size_bytes = RUNTIME_CONFIG_API_KEYS_SIZE_BYTES,
};

static void runtime_copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    size_t copy_len = strlen(src);
    if (copy_len >= dest_size) {
        copy_len = dest_size - 1;
    }

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static bool runtime_get_bool(cJSON *object, const char *key, bool *out_value)
{
    if (object == NULL || key == NULL || out_value == NULL) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *out_value = cJSON_IsTrue(item);
    return true;
}

static bool runtime_get_uint16(cJSON *object, const char *key, uint16_t *out_value)
{
    if (object == NULL || key == NULL || out_value == NULL) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    if (item->valuedouble < 0 || item->valuedouble > UINT16_MAX) {
        return false;
    }

    *out_value = (uint16_t)item->valuedouble;
    return true;
}

static bool runtime_get_uint32(cJSON *object, const char *key, uint32_t *out_value)
{
    if (object == NULL || key == NULL || out_value == NULL) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return false;
    }

    *out_value = (uint32_t)item->valuedouble;
    return true;
}

static bool runtime_get_string(cJSON *object, const char *key, char *dest, size_t dest_size)
{
    if (object == NULL || key == NULL || dest == NULL || dest_size == 0) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    runtime_copy_string(dest, dest_size, item->valuestring);
    return true;
}

static esp_err_t runtime_parse_overrides(void)
{
    if (s_overrides.parsed) {
        return s_overrides.status;
    }

    const uint8_t *start = config_runtime_services_json_start;
    const uint8_t *end = config_runtime_services_json_end;
    size_t length = (size_t)(end - start);

    if (length == 0) {
        s_overrides.parsed = true;
        s_overrides.status = ESP_ERR_NOT_FOUND;
        return s_overrides.status;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)start, length);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse config/runtime/services.json");
        s_overrides.parsed = true;
        s_overrides.status = ESP_ERR_INVALID_STATE;
        return s_overrides.status;
    }

    cJSON *services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (cJSON_IsObject(services)) {
        s_overrides.enable_http_server_present = runtime_get_bool(services, "enable_http_server", &s_overrides.enable_http_server);
        s_overrides.enable_mqtt_present = runtime_get_bool(services, "enable_mqtt", &s_overrides.enable_mqtt);
        s_overrides.enable_mdns_present = runtime_get_bool(services, "enable_mdns", &s_overrides.enable_mdns);
        s_overrides.enable_time_sync_present = runtime_get_bool(services, "enable_time_sync", &s_overrides.enable_time_sync);
        s_overrides.require_time_sync_for_tls_present = runtime_get_bool(services,
                                                                         "require_time_sync_for_tls",
                                                                         &s_overrides.require_time_sync_for_tls);
        s_overrides.https_port_present = runtime_get_uint16(services, "https_port", &s_overrides.https_port);
    }

    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        s_overrides.mqtt_interval_present = runtime_get_uint32(mqtt, "publish_interval_sec", &s_overrides.mqtt_interval_sec);
        s_overrides.mqtt_broker_uri_present = runtime_get_string(mqtt, "broker_uri", s_overrides.mqtt_broker_uri, sizeof(s_overrides.mqtt_broker_uri));
        s_overrides.mqtt_username_present = runtime_get_string(mqtt, "username", s_overrides.mqtt_username, sizeof(s_overrides.mqtt_username));
        s_overrides.mqtt_password_present = runtime_get_string(mqtt, "password", s_overrides.mqtt_password, sizeof(s_overrides.mqtt_password));
    }

    cJSON *telemetry = cJSON_GetObjectItemCaseSensitive(root, "telemetry");
    if (cJSON_IsObject(telemetry)) {
        s_overrides.telemetry_interval_present =
            runtime_get_uint32(telemetry, "publish_interval_sec", &s_overrides.telemetry_publish_interval_sec);
        s_overrides.telemetry_busy_backoff_present =
            runtime_get_uint32(telemetry, "busy_backoff_ms", &s_overrides.telemetry_busy_backoff_ms);
        s_overrides.telemetry_client_backoff_present =
            runtime_get_uint32(telemetry, "client_backoff_ms", &s_overrides.telemetry_client_backoff_ms);
        s_overrides.telemetry_idle_delay_present =
            runtime_get_uint32(telemetry, "idle_delay_ms", &s_overrides.telemetry_idle_delay_ms);
    } else if (s_overrides.mqtt_interval_present) {
        ESP_LOGW(TAG, "mqtt.publish_interval_sec is deprecated; move to telemetry.publish_interval_sec");
    }

    cJSON *dashboard = cJSON_GetObjectItemCaseSensitive(root, "dashboard");
    if (cJSON_IsObject(dashboard)) {
        s_overrides.mdns_hostname_present = runtime_get_string(dashboard, "mdns_hostname", s_overrides.mdns_hostname, sizeof(s_overrides.mdns_hostname));
        s_overrides.mdns_instance_present = runtime_get_string(dashboard, "mdns_instance_name", s_overrides.mdns_instance, sizeof(s_overrides.mdns_instance));
    }

    cJSON *time_sync = cJSON_GetObjectItemCaseSensitive(root, "time_sync");
    if (cJSON_IsObject(time_sync)) {
        s_overrides.timezone_present = runtime_get_string(time_sync, "timezone", s_overrides.timezone, sizeof(s_overrides.timezone));
        s_overrides.time_sync_timeout_present = runtime_get_uint32(time_sync, "timeout_sec", &s_overrides.time_sync_timeout_sec);
        s_overrides.time_sync_retry_attempts_present = runtime_get_uint32(time_sync, "retry_attempts", &s_overrides.time_sync_retry_attempts);
        s_overrides.time_sync_retry_delay_present = runtime_get_uint32(time_sync, "retry_delay_sec", &s_overrides.time_sync_retry_delay_sec);
    }

    cJSON_Delete(root);
    s_overrides.parsed = true;
    s_overrides.status = ESP_OK;
    return s_overrides.status;
}

esp_err_t runtime_config_apply_services_overrides(services_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t parse_result = runtime_parse_overrides();
    if (parse_result == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }

    if (parse_result != ESP_OK) {
        return parse_result;
    }

    if (s_overrides.enable_http_server_present) {
        config->enable_http_server = s_overrides.enable_http_server;
    }

    if (s_overrides.enable_mqtt_present) {
        config->enable_mqtt = s_overrides.enable_mqtt;
    }

    if (s_overrides.enable_mdns_present) {
        config->enable_mdns = s_overrides.enable_mdns;
    }

    if (s_overrides.enable_time_sync_present) {
        config->enable_time_sync = s_overrides.enable_time_sync;
    }

    if (s_overrides.require_time_sync_for_tls_present) {
        config->require_time_sync_for_tls = s_overrides.require_time_sync_for_tls;
    }

    if (s_overrides.https_port_present) {
        config->https_port = s_overrides.https_port;
    }

    if (s_overrides.telemetry_interval_present) {
        config->mqtt_publish_interval_sec = s_overrides.telemetry_publish_interval_sec;
    } else if (s_overrides.mqtt_interval_present) {
        config->mqtt_publish_interval_sec = s_overrides.mqtt_interval_sec;
    }

    if (s_overrides.telemetry_busy_backoff_present) {
        config->telemetry_busy_backoff_ms = s_overrides.telemetry_busy_backoff_ms;
    }

    if (s_overrides.telemetry_client_backoff_present) {
        config->telemetry_client_backoff_ms = s_overrides.telemetry_client_backoff_ms;
    }

    if (s_overrides.telemetry_idle_delay_present) {
        config->telemetry_idle_delay_ms = s_overrides.telemetry_idle_delay_ms;
    }

    if (s_overrides.time_sync_timeout_present) {
        config->time_sync_timeout_sec = s_overrides.time_sync_timeout_sec;
    }

    if (s_overrides.time_sync_retry_attempts_present) {
        uint32_t attempts = s_overrides.time_sync_retry_attempts;
        if (attempts > UINT8_MAX) {
            attempts = UINT8_MAX;
        }
        config->time_sync_retry_attempts = (uint8_t)attempts;
    }

    if (s_overrides.time_sync_retry_delay_present) {
        config->time_sync_retry_delay_sec = s_overrides.time_sync_retry_delay_sec;
    }

    if (s_overrides.mqtt_broker_uri_present) {
        config->mqtt_broker_uri = s_overrides.mqtt_broker_uri;
    }

    if (s_overrides.mqtt_username_present) {
        config->mqtt_username = s_overrides.mqtt_username;
    }

    if (s_overrides.mqtt_password_present) {
        config->mqtt_password = s_overrides.mqtt_password;
    }

    if (s_overrides.mdns_hostname_present) {
        config->mdns_hostname = s_overrides.mdns_hostname;
    }

    if (s_overrides.mdns_instance_present) {
        config->mdns_instance_name = s_overrides.mdns_instance;
    }

    if (s_overrides.timezone_present) {
        config->timezone = s_overrides.timezone;
    }

    return ESP_OK;
}

const runtime_config_digest_t *runtime_config_get_digest(void)
{
    return &s_runtime_digests;
}
