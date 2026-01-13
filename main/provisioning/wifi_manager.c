#include "wifi_manager.h"
#include "provisioning/provisioning_state.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "PROV:WIFI_MGR";

// WiFi event bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/**
 * WiFi Connection Retry Configuration
 *
 * Implements exponential backoff: delay = base_delay * 2^attempt
 * Example progression: 1s, 2s, 4s, 8s, 16s, 32s, 60s (capped)
 *
 * Rationale:
 * - 1s base: Quick retry for transient network issues
 * - 60s max: Prevents excessive traffic on failed networks
 * - Exponent 6: Balances between quick recovery and backoff
 */
#define WIFI_RETRY_BASE_DELAY_MS 1000U      // Initial retry delay (1 second)
#define WIFI_RETRY_MAX_DELAY_MS 60000U      // Maximum retry delay (60 seconds)
#define WIFI_RETRY_MAX_EXPONENT 6U          // Max exponent for backoff (2^6 = 64s, capped to 60s)

/**
 * Error reporting interval
 * Report WiFi failure status every N retry attempts to avoid log spam
 */
#define WIFI_ERROR_REPORT_INTERVAL 10       // Report failure state every 10 retries

static EventGroupHandle_t s_wifi_event_group;
static TimerHandle_t s_retry_timer = NULL;
static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_has_credentials_configured = false;

// Store credentials temporarily before saving
static char pending_ssid[33] = {0};
static char pending_password[64] = {0};

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_retry_timer_cb(TimerHandle_t timer);
static uint32_t wifi_compute_retry_delay_ms(void);
static void wifi_schedule_retry(uint32_t delay_ms);

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, attempting to connect...");
        // Only connect if we have credentials configured
        if (s_has_credentials_configured) {
            esp_wifi_connect();
        }
        
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconn_event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGI(TAG, "WiFi disconnected (reason: %d)", disconn_event->reason);
        
        s_is_connected = false;
        s_retry_num++;
        const uint32_t delay_ms = wifi_compute_retry_delay_ms();
        wifi_schedule_retry(delay_ms);
        
        char msg[96];
        snprintf(msg, sizeof(msg), "Connecting... (attempt %d, next in %lus)",
                 s_retry_num, (unsigned long)(delay_ms / 1000U));
        provisioning_state_set(PROV_STATE_WIFI_CONNECTING, STATUS_SUCCESS, msg);
        
        provisioning_status_code_t status_code = STATUS_ERROR_WIFI_TIMEOUT;
        const char* error_msg = "Connection timeout";

        switch (disconn_event->reason) {
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                status_code = STATUS_ERROR_WIFI_AUTH_FAILED;
                error_msg = "Authentication failed - check password";
                break;
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_BEACON_TIMEOUT:
                status_code = STATUS_ERROR_WIFI_NO_AP_FOUND;
                error_msg = "Access point not found - check SSID";
                break;
            default:
                break;
        }

        if (s_retry_num == 1) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else if (s_retry_num % WIFI_ERROR_REPORT_INTERVAL == 0) {
            provisioning_state_set(PROV_STATE_WIFI_FAILED, status_code, error_msg);
        }
        
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
        if (s_retry_timer != NULL) {
            xTimerStop(s_retry_timer, 0);
        }
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // ESP-IDF automatically saves credentials with WIFI_STORAGE_FLASH
        ESP_LOGI(TAG, "Credentials saved automatically by ESP-IDF (encrypted in NVS)");
        
        // Clear sensitive data from memory
        memset(pending_ssid, 0, sizeof(pending_ssid));
        memset(pending_password, 0, sizeof(pending_password));
        
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        
        provisioning_state_set(PROV_STATE_PROVISIONED, STATUS_SUCCESS, ip_str);
    }
}

// Note: Credential storage is now handled by ESP-IDF via WIFI_STORAGE_FLASH
// No custom NVS storage needed - ESP-IDF encrypts credentials automatically

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi manager");
    
    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }
    
    // Create retry timer before network activity
    s_retry_timer = xTimerCreate("wifi_retry",
                                 pdMS_TO_TICKS(WIFI_RETRY_BASE_DELAY_MS),
                                 pdFALSE,
                                 NULL,
                                 wifi_retry_timer_cb);
    if (s_retry_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi retry timer");
        return ESP_FAIL;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default WiFi station
    esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Use ESP-IDF's built-in NVS storage for WiFi credentials
    // This works seamlessly with wifi_prov_mgr
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    
    // Set WiFi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi manager initialized successfully");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    if (ssid == NULL || password == NULL) {
        ESP_LOGE(TAG, "SSID or password is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
    
    // Store credentials temporarily
    strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
    strncpy(pending_password, password, sizeof(pending_password) - 1);
    
    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    // Stop WiFi if already running
    esp_wifi_stop();
    
    // Set configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Mark that we have credentials configured
    s_has_credentials_configured = true;
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Reset retry counter
    s_retry_num = 0;
    if (s_retry_timer != NULL) {
        xTimerStop(s_retry_timer, 0);
    }
    
    // Update state
    provisioning_state_set(PROV_STATE_WIFI_CONNECTING, STATUS_SUCCESS, "Initiating WiFi connection");
    
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

esp_err_t wifi_manager_get_stored_credentials(char* ssid, char* password)
{
    // Use ESP-IDF's built-in WiFi config storage
    wifi_config_t wifi_cfg;
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Check if SSID is configured
    if (wifi_cfg.sta.ssid[0] == '\0') {
        ESP_LOGI(TAG, "No stored WiFi credentials found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Copy credentials to output buffers
    strncpy(ssid, (char*)wifi_cfg.sta.ssid, 33);
    strncpy(password, (char*)wifi_cfg.sta.password, 64);
    
    ESP_LOGD(TAG, "Retrieved stored credentials for SSID: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from WiFi");
    s_is_connected = false;
    if (s_retry_timer != NULL) {
        xTimerStop(s_retry_timer, 0);
    }
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_clear_credentials(void)
{
    ESP_LOGI(TAG, "Clearing stored credentials");

    // Stop WiFi first (ignore if already stopped/not initialized)
    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK &&
        stop_ret != ESP_ERR_WIFI_NOT_INIT &&
        stop_ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(stop_ret));
    }

    // Use esp_wifi_restore so the driver clears its own flash-stored config
    esp_err_t ret = esp_wifi_restore();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_restore failed (%s), falling back to manual NVS erase",
                 esp_err_to_name(ret));

        nvs_handle_t nvs_handle;
        esp_err_t nvs_ret = nvs_open("net80211", NVS_READWRITE, &nvs_handle);
        if (nvs_ret == ESP_OK) {
            nvs_ret = nvs_erase_all(nvs_handle);
            if (nvs_ret == ESP_OK) {
                nvs_ret = nvs_commit(nvs_handle);
                ESP_LOGI(TAG, "Erased WiFi credentials from NVS namespace 'net80211'");
            } else {
                ESP_LOGW(TAG, "Failed to erase NVS WiFi namespace: %s", esp_err_to_name(nvs_ret));
            }
            nvs_close(nvs_handle);
            ret = nvs_ret;
        } else {
            ESP_LOGW(TAG, "Failed to open NVS WiFi namespace (may not exist): %s",
                     esp_err_to_name(nvs_ret));
            ret = (nvs_ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : nvs_ret;
        }
    } else {
        ESP_LOGI(TAG, "esp_wifi_restore succeeded; WiFi config reset to defaults");
    }

    // Reset local state regardless of restore outcome
    s_has_credentials_configured = false;
    s_is_connected = false;
    memset(pending_ssid, 0, sizeof(pending_ssid));
    memset(pending_password, 0, sizeof(pending_password));

    if (s_retry_timer != NULL) {
        xTimerStop(s_retry_timer, 0);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Credentials cleared successfully");
    } else {
        ESP_LOGE(TAG, "Failed to clear credentials: %s", esp_err_to_name(ret));
    }

    return ret;
}

static void wifi_retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!s_has_credentials_configured) {
        ESP_LOGW(TAG, "Retry timer fired but no credentials configured");
        return;
    }

    ESP_LOGI(TAG, "WiFi retry timer firing (attempt %d)", s_retry_num);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
}

static uint32_t wifi_compute_retry_delay_ms(void)
{
    uint32_t exponent = (s_retry_num < WIFI_RETRY_MAX_EXPONENT) ? (uint32_t)s_retry_num : WIFI_RETRY_MAX_EXPONENT;
    uint32_t delay = WIFI_RETRY_BASE_DELAY_MS;
    delay <<= exponent;
    if (delay > WIFI_RETRY_MAX_DELAY_MS) {
        delay = WIFI_RETRY_MAX_DELAY_MS;
    }
    return delay;
}

static void wifi_schedule_retry(uint32_t delay_ms)
{
    if (s_retry_timer == NULL) {
        ESP_LOGW(TAG, "Retry timer not initialized; connecting immediately");
        esp_wifi_connect();
        return;
    }

    if (delay_ms == 0) {
        delay_ms = WIFI_RETRY_BASE_DELAY_MS;
    }

    xTimerStop(s_retry_timer, 0);
    xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(delay_ms), 0);
    xTimerStart(s_retry_timer, 0);
}

esp_err_t wifi_manager_save_credentials(const char* ssid, const char* password)
{
    // With WIFI_STORAGE_FLASH, ESP-IDF automatically saves credentials
    // This function is kept for API compatibility but does nothing
    // Credentials are saved automatically when esp_wifi_set_config() is called
    ESP_LOGI(TAG, "Credentials for SSID '%s' will be saved automatically by ESP-IDF", ssid);
    return ESP_OK;
}
