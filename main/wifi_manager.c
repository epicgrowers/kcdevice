#include "wifi_manager.h"
#include "provisioning/provisioning_state.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

// WiFi event bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Maximum retry attempts
#define MAX_RETRY_ATTEMPTS 5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_has_credentials_configured = false;

// Store credentials temporarily before saving
static char pending_ssid[33] = {0};
static char pending_password[64] = {0};

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

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
        
        if (s_retry_num < MAX_RETRY_ATTEMPTS) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connection attempt %d/%d", s_retry_num, MAX_RETRY_ATTEMPTS);
            
            char msg[64];
            snprintf(msg, sizeof(msg), "Connecting... (attempt %d/%d)", s_retry_num, MAX_RETRY_ATTEMPTS);
            provisioning_state_set(PROV_STATE_WIFI_CONNECTING, STATUS_SUCCESS, msg);
            
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d attempts", MAX_RETRY_ATTEMPTS);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            
            // Determine failure reason
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
            
            provisioning_state_set(PROV_STATE_WIFI_FAILED, status_code, error_msg);
        }
        
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
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
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_clear_credentials(void)
{
    ESP_LOGI(TAG, "Clearing stored credentials");
    
    // Stop WiFi first
    esp_wifi_stop();
    
    // Clear ESP-IDF's WiFi credentials by erasing the NVS WiFi namespace
    // This is the proper way to clear credentials stored by WIFI_STORAGE_FLASH
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("nvs.net80211", NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_erase_all(nvs_handle);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Erased WiFi credentials from NVS namespace 'nvs.net80211'");
        } else {
            ESP_LOGW(TAG, "Failed to erase NVS WiFi namespace: %s", esp_err_to_name(ret));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS WiFi namespace (may not exist): %s", esp_err_to_name(ret));
        // Not an error - credentials might not exist yet
        ret = ESP_OK;
    }
    
    // Clear the configured flag
    s_has_credentials_configured = false;
    
    ESP_LOGI(TAG, "Credentials cleared successfully");
    return ret;
}

esp_err_t wifi_manager_save_credentials(const char* ssid, const char* password)
{
    // With WIFI_STORAGE_FLASH, ESP-IDF automatically saves credentials
    // This function is kept for API compatibility but does nothing
    // Credentials are saved automatically when esp_wifi_set_config() is called
    ESP_LOGI(TAG, "Credentials for SSID '%s' will be saved automatically by ESP-IDF", ssid);
    return ESP_OK;
}
