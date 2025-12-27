#include "network_boot.h"

#include "api_key_manager.h"
#include "boot/boot_config.h"
#include "boot/boot_monitor.h"
#include "cloud_provisioning.h"
#include "services/core/services.h"
#include "services/time_sync/time_sync.h"
#include "provisioning/provisioning_runner.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include <time.h>
#include <string.h>

static const char *TAG = "NETWORK_BOOT";

static network_boot_config_t s_network_boot_config;
static bool s_network_task_started = false;

typedef struct {
    bool core_ready;
    bool core_degraded;
    bool http_ready;
    bool mdns_ready;
    bool mqtt_ready;
    bool http_degraded;
    bool mdns_degraded;
    bool mqtt_degraded;
} services_observer_t;

static services_observer_t s_services_observer = {0};

static void log_time_sync(bool synced, struct tm *current_time)
{
    if (synced && current_time != NULL) {
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", current_time);
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Time Synchronized Successfully!");
        ESP_LOGI(TAG, "Current time: %s UTC", time_str);
        ESP_LOGI(TAG, "====================================");
    } else {
        ESP_LOGW(TAG, "Time synchronization failed");
    }
}

static void log_cloud_provision(bool success, const char *message)
{
    if (success) {
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Cloud Provisioning Successful!");
        ESP_LOGI(TAG, "Message: %s", message ? message : "N/A");
        ESP_LOGI(TAG, "====================================");
    } else {
        ESP_LOGW(TAG, "====================================");
        ESP_LOGW(TAG, "Cloud Provisioning Failed");
        ESP_LOGW(TAG, "Error: %s", message ? message : "Unknown");
        ESP_LOGW(TAG, "====================================");
    }
}

static void services_status_listener(const services_status_event_t *event, void *ctx)
{
    if (event == NULL) {
        return;
    }

    services_observer_t *observer = (services_observer_t *)ctx;
    const char *component = (event->component != NULL) ? event->component : "core";

    switch (event->type) {
        case SERVICES_EVENT_STARTING:
            ESP_LOGI(TAG, "SERVICES: %s starting", component);
            break;
        case SERVICES_EVENT_READY:
            ESP_LOGI(TAG, "SERVICES: %s ready", component);
            if (observer != NULL) {
                if (event->component == NULL) {
                    observer->core_ready = true;
                } else if (strcmp(component, "http") == 0) {
                    observer->http_ready = true;
                } else if (strcmp(component, "mdns") == 0) {
                    observer->mdns_ready = true;
                } else if (strcmp(component, "mqtt") == 0) {
                    observer->mqtt_ready = true;
                }
            }
            break;
        case SERVICES_EVENT_DEGRADED:
            ESP_LOGW(TAG, "SERVICES: %s degraded (%s)", component, esp_err_to_name(event->result));
            if (observer != NULL) {
                observer->core_degraded = true;
                if (strcmp(component, "http") == 0) {
                    observer->http_degraded = true;
                } else if (strcmp(component, "mdns") == 0) {
                    observer->mdns_degraded = true;
                } else if (strcmp(component, "mqtt") == 0) {
                    observer->mqtt_degraded = true;
                }
            }
            break;
        case SERVICES_EVENT_STOPPED:
            ESP_LOGI(TAG, "SERVICES: %s stopped", component);
            if (observer != NULL && event->component == NULL) {
                observer->core_ready = false;
            }
            break;
        default:
            break;
    }
}

static void network_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK TASK: Starting");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret;
    bool tls_ready = false;
    bool time_synced = false;

    if (!provisioning_wifi_is_connected()) {
        ESP_LOGW(TAG, "NETWORK: WiFi not connected, waiting...");
        int wait_time = 0;
        while (!provisioning_wifi_is_connected() && wait_time < (WIFI_CONNECT_TIMEOUT_MS / 1000)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_time++;
        }

        if (!provisioning_wifi_is_connected()) {
            ESP_LOGW(TAG, "NETWORK: WiFi connection timeout, network services unavailable");
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGI(TAG, "NETWORK: \u2713 WiFi connected");

    ESP_LOGI(TAG, "NETWORK: Initializing NTP time synchronization...");
    ret = time_sync_init(NULL, log_time_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NETWORK: Failed to initialize time sync: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "NETWORK: Waiting for time synchronization...");
    int sync_wait = 0;
    while (!time_sync_is_synced() && sync_wait < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sync_wait++;
    }

    time_synced = time_sync_is_synced();
    if (!time_synced) {
        ESP_LOGW(TAG, "NETWORK: Time sync timeout, TLS may fail");
    } else {
        ESP_LOGI(TAG, "NETWORK: Time synchronized successfully");
    }

    ESP_LOGI(TAG, "NETWORK: Initializing API key manager...");
    api_key_manager_init();

    ESP_LOGI(TAG, "NETWORK: Initializing cloud provisioning...");
    cloud_prov_init(log_cloud_provision);

    ESP_LOGI(TAG, "NETWORK: Starting cloud provisioning (fetching certificates)...");
    ret = cloud_prov_provision_device();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NETWORK: Cloud provisioning failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "NETWORK: Retrying cloud provisioning in %dms...", CLOUD_PROVISION_RETRY_MS);

        vTaskDelay(pdMS_TO_TICKS(CLOUD_PROVISION_RETRY_MS));
        ret = cloud_prov_provision_device();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NETWORK: Cloud provisioning failed after retry, network services unavailable");
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGI(TAG, "NETWORK: \u2713 Cloud provisioning complete (certificates obtained)");

    ESP_LOGI(TAG, "NETWORK: Downloading MQTT CA certificate...");
    ret = cloud_prov_download_mqtt_ca_cert();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NETWORK: Failed to download MQTT CA cert: %s", esp_err_to_name(ret));
    }

    tls_ready = cloud_prov_has_certificates();
    if (!tls_ready) {
        ESP_LOGW(TAG, "NETWORK: TLS certificates not available after provisioning");
    } else {
        ESP_LOGI(TAG, "NETWORK: TLS certificates verified");
    }

    const char *mqtt_broker = "mqtts://mqtt.kannacloud.com:8883";
    const char *mqtt_username = "sensor01";
    const char *mqtt_password = "xkKKYQWxiT83Ni3";

    memset(&s_services_observer, 0, sizeof(s_services_observer));

    services_config_t services_cfg = {
        .enable_http_server = tls_ready,
        .enable_mqtt = tls_ready,
        .enable_mdns = tls_ready,
        .enable_time_sync = time_synced,
        .tls_ready = tls_ready,
        .time_synced = time_synced,
        .https_port = 443,
        .mqtt_publish_interval_sec = 0,
        .mqtt_broker_uri = mqtt_broker,
        .mqtt_username = mqtt_username,
        .mqtt_password = mqtt_password,
        .mdns_hostname = "kc",
        .mdns_instance_name = "KannaCloud Device",
        .timezone = NULL,
        .time_sync_callback = NULL,
        .time_sync_timeout_sec = 10,
    };

#ifdef CONFIG_IDF_TARGET_ESP32C6
    services_cfg.enable_http_server = false;
    services_cfg.enable_mdns = false;
#endif

    services_dependencies_t services_deps = {
        .boot_event_group = s_network_boot_config.event_group,
        .network_ready_bit = s_network_boot_config.ready_bit,
        .sensor_pipeline_ctx = NULL,
    };

    ret = services_start(&services_cfg, &services_deps, services_status_listener, &s_services_observer);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NETWORK: \u2713 Services core launch requested");
    } else {
        ESP_LOGE(TAG, "NETWORK: Services core failed to start: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK: \u2713 All network services started");
    ESP_LOGI(TAG, "========================================");

    char network_detail[160];
#ifndef CONFIG_IDF_TARGET_ESP32C6
    const char *https_detail = "skipped";
    if (services_cfg.enable_http_server) {
        if (s_services_observer.http_ready) {
            https_detail = "enabled";
        } else if (s_services_observer.http_degraded) {
            https_detail = "degraded";
        } else if (!services_cfg.tls_ready) {
            https_detail = "skipped";
        } else {
            https_detail = "pending";
        }
    } else {
        https_detail = "disabled";
    }

    const char *mqtt_detail = services_cfg.tls_ready ? "disabled" : "skipped";
    if (services_cfg.enable_mqtt) {
        if (s_services_observer.mqtt_ready) {
            mqtt_detail = "ready";
        } else if (s_services_observer.mqtt_degraded) {
            mqtt_detail = "degraded";
        } else if (!services_cfg.tls_ready) {
            mqtt_detail = "skipped";
        } else {
            mqtt_detail = "pending";
        }
    }

    snprintf(network_detail, sizeof(network_detail),
             "tls=%s ntp=%s mqtt=%s https=%s",
             tls_ready ? "ready" : "missing",
             time_synced ? "synced" : "timeout",
             mqtt_detail,
             https_detail);
#else
    const char *mqtt_detail = services_cfg.enable_mqtt
                                  ? (s_services_observer.mqtt_ready
                                         ? "ready"
                                         : (s_services_observer.mqtt_degraded ? "degraded"
                                                                               : "pending"))
                                  : (services_cfg.tls_ready ? "disabled" : "skipped");

    snprintf(network_detail, sizeof(network_detail),
             "tls=%s ntp=%s mqtt=%s https=disabled",
             tls_ready ? "ready" : "missing",
             time_synced ? "synced" : "timeout",
             mqtt_detail);
#endif
    boot_monitor_publish("network_ready", network_detail);

    vTaskDelete(NULL);
}

esp_err_t network_boot_start(const network_boot_config_t *config)
{
    if (config == NULL || config->event_group == NULL || config->ready_bit == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_network_task_started) {
        return ESP_OK;
    }

    s_network_boot_config = *config;

    BaseType_t task_ret = xTaskCreate(
        network_task,
        "network_task",
        NETWORK_TASK_STACK_SIZE,
        NULL,
        NETWORK_TASK_PRIORITY,
        NULL);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network task");
        return ESP_FAIL;
    }

    s_network_task_started = true;
    ESP_LOGI(TAG, "NETWORK: network_task launched (priority %d)", NETWORK_TASK_PRIORITY);
    return ESP_OK;
}
