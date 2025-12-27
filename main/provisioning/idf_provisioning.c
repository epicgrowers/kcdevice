#include "provisioning/idf_provisioning.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning/provisioning_state.h"
#include "wifi_manager.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "idf_prov";
static const char *kServicePrefix = "kc-";
static const char *kPop = "sumppop";

static bool provisioning_active;
static char service_name[24];

static void provisioning_event_handler(void *user_data, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data);

esp_err_t idf_provisioning_start(void) {
    if (provisioning_active) {
        ESP_LOGW(TAG, "Provisioning already running");
        return ESP_OK;
    }

    // Initialize WiFi if not already initialized
    // wifi_prov_mgr requires WiFi to be initialized first
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        ESP_LOGI(TAG, "WiFi not initialized, initializing for provisioning");
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    } else {
        ESP_LOGI(TAG, "WiFi already initialized, using existing configuration");
    }

    const char *adv_name = idf_provisioning_get_service_name();
    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .wifi_prov_conn_cfg = {
            .wifi_conn_attempts = 3,  // Let prov manager handle WiFi connection (3 attempts)
        },
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_cfg));

    // Register only WIFI_PROV_EVENT handler (not WiFi events - prov manager handles those)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               provisioning_event_handler, NULL));

    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    wifi_prov_security1_params_t *sec_params = (wifi_prov_security1_params_t *)idf_provisioning_get_pop();

    esp_err_t err = wifi_prov_mgr_start_provisioning(security, sec_params, adv_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning start failed: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return err;
    }

    provisioning_active = true;
    provisioning_state_set(PROV_STATE_BLE_CONNECTED, STATUS_SUCCESS, "BLE ready");
    ESP_LOGI(TAG, "Provisioning started (service %s)", adv_name);
    return ESP_OK;
}

void idf_provisioning_stop(void) {
    if (!provisioning_active) {
        return;
    }

    provisioning_active = false;

    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();

    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_event_handler);

    ESP_LOGI(TAG, "Provisioning stopped");
}

bool idf_provisioning_is_running(void) {
    return provisioning_active;
}

const char *idf_provisioning_get_service_name(void) {
    if (!service_name[0]) {
        uint8_t mac[6];
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
        snprintf(service_name, sizeof(service_name), "%s%02X%02X%02X", kServicePrefix,
                 mac[3], mac[4], mac[5]);
    }
    return service_name;
}

const char *idf_provisioning_get_pop(void) {
    return kPop;
}

static void provisioning_event_handler(void *user_data, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    switch (event_id) {
        case WIFI_PROV_START:
            provisioning_state_set(PROV_STATE_BLE_CONNECTED, STATUS_SUCCESS, "Waiting for app");
            break;
        case WIFI_PROV_CRED_RECV: {
            // Provisioning manager will handle WiFi connection automatically
            // Credentials will be saved automatically by ESP-IDF with WIFI_STORAGE_FLASH
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials for SSID: %s (will be saved automatically)", wifi_sta_cfg->ssid);
            
            provisioning_state_set(PROV_STATE_CREDENTIALS_RECEIVED, STATUS_SUCCESS,
                                   "Credentials received");
            break;
        }
        case WIFI_PROV_CRED_FAIL:
            provisioning_state_set(PROV_STATE_WIFI_FAILED, STATUS_ERROR_WIFI_AUTH_FAILED,
                                   "AP rejected credentials");
            break;
        case WIFI_PROV_CRED_SUCCESS: {
            // WiFi connected successfully, credentials automatically saved by ESP-IDF
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
                ESP_LOGI(TAG, "WiFi connected successfully to SSID: %s (credentials saved automatically)", wifi_cfg.sta.ssid);
            }
            
            provisioning_state_set(PROV_STATE_WIFI_CONNECTING, STATUS_SUCCESS,
                                   "Connecting to AP");
            break;
        }
        case WIFI_PROV_END:
            // Add delay to ensure success response is sent to Android app before closing BLE
            ESP_LOGI(TAG, "Provisioning complete, waiting 2 seconds before BLE cleanup...");
            vTaskDelay(pdMS_TO_TICKS(2000));  // Increased to 2 seconds for Android app
            ESP_LOGI(TAG, "Stopping provisioning and cleaning up BLE...");
            idf_provisioning_stop();
            break;
        default:
            ESP_LOGW(TAG, "Unhandled provisioning event %ld", event_id);
            break;
    }
}
