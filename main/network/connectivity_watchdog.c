#include "network/connectivity_watchdog.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "provisioning/provisioning_runner.h"
#include "services/core/services.h"
#include <limits.h>

static const char *TAG = "CONN_WATCH";

#define CONNECTIVITY_WATCHDOG_STACK   3072
#define CONNECTIVITY_WATCHDOG_PRIO    4
#define CONNECTIVITY_WIFI_POLL_MS     1000U

static TaskHandle_t s_watchdog_task = NULL;
static connectivity_watchdog_config_t s_config = {
    .check_interval_ms = 10000U,
    .failure_threshold = 3U,
    .probe_hostname = "time.google.com",
};
static bool s_wan_degraded = false;
static uint32_t s_failure_count = 0;

static bool connectivity_watchdog_probe(void)
{
    const char *host = (s_config.probe_hostname != NULL) ? s_config.probe_hostname : "time.google.com";
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, NULL, &hints, &res);
    if (res != NULL) {
        freeaddrinfo(res);
    }
    return err == 0;
}

static void connectivity_watchdog_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Connectivity watchdog started (interval=%ums, threshold=%u)",
             (unsigned)s_config.check_interval_ms,
             (unsigned)s_config.failure_threshold);

    while (true) {
        if (!provisioning_wifi_is_connected()) {
            s_failure_count = 0;
            if (s_wan_degraded) {
                ESP_LOGI(TAG, "Wi-Fi offline; clearing WAN degraded flag");
                s_wan_degraded = false;
            }
            vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_WIFI_POLL_MS));
            continue;
        }

        bool reachable = connectivity_watchdog_probe();
        if (!reachable) {
            if (s_failure_count < UINT32_MAX) {
                s_failure_count++;
            }
            if (!s_wan_degraded &&
                s_config.failure_threshold > 0 &&
                s_failure_count >= s_config.failure_threshold) {
                s_wan_degraded = true;
                ESP_LOGW(TAG, "WAN probe failed %u times; reporting degraded", (unsigned)s_failure_count);
                services_report_degraded("connectivity_watchdog", ESP_ERR_INVALID_RESPONSE);
            }
        } else {
            if (s_wan_degraded) {
                ESP_LOGI(TAG, "WAN reachability restored; requesting service recovery");
                s_wan_degraded = false;
                esp_err_t rec = services_handle_network_recovered();
                if (rec != ESP_OK) {
                    ESP_LOGW(TAG, "Network recovery handler returned %s", esp_err_to_name(rec));
                }
            }
            s_failure_count = 0;
        }

        uint32_t interval = (s_config.check_interval_ms > 0) ? s_config.check_interval_ms : 10000U;
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

esp_err_t connectivity_watchdog_start(const connectivity_watchdog_config_t *config)
{
    if (s_watchdog_task != NULL) {
        return ESP_OK;
    }

    if (config != NULL) {
        s_config.check_interval_ms = config->check_interval_ms;
        s_config.failure_threshold = config->failure_threshold;
        s_config.probe_hostname = config->probe_hostname;
    }

#ifdef CONFIG_FREERTOS_UNICORE
    BaseType_t ret = xTaskCreate(
        connectivity_watchdog_task,
        "conn_watch",
        CONNECTIVITY_WATCHDOG_STACK,
        NULL,
        CONNECTIVITY_WATCHDOG_PRIO,
        &s_watchdog_task);
#else
    BaseType_t ret = xTaskCreatePinnedToCore(
        connectivity_watchdog_task,
        "conn_watch",
        CONNECTIVITY_WATCHDOG_STACK,
        NULL,
        CONNECTIVITY_WATCHDOG_PRIO,
        &s_watchdog_task,
        0);
#endif

    if (ret != pdPASS) {
        s_watchdog_task = NULL;
        ESP_LOGE(TAG, "Failed to start connectivity watchdog task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t connectivity_watchdog_stop(void)
{
    if (s_watchdog_task == NULL) {
        return ESP_OK;
    }

    TaskHandle_t handle = s_watchdog_task;
    s_watchdog_task = NULL;
    ESP_LOGI(TAG, "Stopping connectivity watchdog");
    vTaskDelete(handle);
    return ESP_OK;
}
