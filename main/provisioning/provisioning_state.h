#ifndef PROVISIONING_STATE_H
#define PROVISIONING_STATE_H

#include <stdint.h>

// Provisioning states
typedef enum {
    PROV_STATE_IDLE = 0,
    PROV_STATE_BLE_CONNECTED,
    PROV_STATE_CREDENTIALS_RECEIVED,
    PROV_STATE_WIFI_CONNECTING,
    PROV_STATE_WIFI_CONNECTED,
    PROV_STATE_WIFI_FAILED,
    PROV_STATE_PROVISIONED,
    PROV_STATE_ERROR
} provisioning_state_t;

// Status codes sent to mobile app
typedef enum {
    STATUS_SUCCESS = 0,
    STATUS_ERROR_INVALID_JSON,
    STATUS_ERROR_MISSING_SSID,
    STATUS_ERROR_MISSING_PASSWORD,
    STATUS_ERROR_WIFI_TIMEOUT,
    STATUS_ERROR_WIFI_AUTH_FAILED,
    STATUS_ERROR_WIFI_NO_AP_FOUND,
    STATUS_ERROR_STORAGE_FAILED
} provisioning_status_code_t;

// Callback function type for state changes
typedef void (*state_change_callback_t)(provisioning_state_t state, provisioning_status_code_t status, const char* message);

/**
 * @brief Initialize the provisioning state machine
 */
void provisioning_state_init(void);

/**
 * @brief Set the current provisioning state
 * 
 * @param state New state
 * @param status Status code
 * @param message Optional message (can be NULL)
 */
void provisioning_state_set(provisioning_state_t state, provisioning_status_code_t status, const char* message);

/**
 * @brief Get the current provisioning state
 * 
 * @return Current state
 */
provisioning_state_t provisioning_state_get(void);

/**
 * @brief Register a callback for state changes
 * 
 * @param callback Callback function
 */
void provisioning_state_register_callback(state_change_callback_t callback);

/**
 * @brief Get string representation of state
 * 
 * @param state State to convert
 * @return String representation
 */
const char* provisioning_state_to_string(provisioning_state_t state);

/**
 * @brief Get string representation of status code
 * 
 * @param status Status code to convert
 * @return String representation
 */
const char* provisioning_status_to_string(provisioning_status_code_t status);

#endif // PROVISIONING_STATE_H
