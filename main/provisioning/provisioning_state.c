#include "provisioning/provisioning_state.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PROV:STATE";

static provisioning_state_t current_state = PROV_STATE_IDLE;
static state_change_callback_t state_callback = NULL;

void provisioning_state_init(void)
{
    current_state = PROV_STATE_IDLE;
    state_callback = NULL;
    ESP_LOGI(TAG, "Provisioning state machine initialized");
}

void provisioning_state_set(provisioning_state_t state, provisioning_status_code_t status, const char* message)
{
    provisioning_state_t old_state = current_state;
    current_state = state;
    
    ESP_LOGI(TAG, "State transition: %s -> %s (Status: %s, Message: %s)",
             provisioning_state_to_string(old_state),
             provisioning_state_to_string(state),
             provisioning_status_to_string(status),
             message ? message : "none");
    
    // Notify callback if registered
    if (state_callback != NULL) {
        state_callback(state, status, message);
    }
}

provisioning_state_t provisioning_state_get(void)
{
    return current_state;
}

void provisioning_state_register_callback(state_change_callback_t callback)
{
    state_callback = callback;
    ESP_LOGI(TAG, "State change callback registered");
}

const char* provisioning_state_to_string(provisioning_state_t state)
{
    switch (state) {
        case PROV_STATE_IDLE:
            return "IDLE";
        case PROV_STATE_BLE_CONNECTED:
            return "BLE_CONNECTED";
        case PROV_STATE_CREDENTIALS_RECEIVED:
            return "CREDENTIALS_RECEIVED";
        case PROV_STATE_WIFI_CONNECTING:
            return "WIFI_CONNECTING";
        case PROV_STATE_WIFI_CONNECTED:
            return "WIFI_CONNECTED";
        case PROV_STATE_WIFI_FAILED:
            return "WIFI_FAILED";
        case PROV_STATE_PROVISIONED:
            return "PROVISIONED";
        case PROV_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

const char* provisioning_status_to_string(provisioning_status_code_t status)
{
    switch (status) {
        case STATUS_SUCCESS:
            return "SUCCESS";
        case STATUS_ERROR_INVALID_JSON:
            return "ERROR_INVALID_JSON";
        case STATUS_ERROR_MISSING_SSID:
            return "ERROR_MISSING_SSID";
        case STATUS_ERROR_MISSING_PASSWORD:
            return "ERROR_MISSING_PASSWORD";
        case STATUS_ERROR_WIFI_TIMEOUT:
            return "ERROR_WIFI_TIMEOUT";
        case STATUS_ERROR_WIFI_AUTH_FAILED:
            return "ERROR_WIFI_AUTH_FAILED";
        case STATUS_ERROR_WIFI_NO_AP_FOUND:
            return "ERROR_WIFI_NO_AP_FOUND";
        case STATUS_ERROR_STORAGE_FAILED:
            return "ERROR_STORAGE_FAILED";
        default:
            return "UNKNOWN_ERROR";
    }
}
