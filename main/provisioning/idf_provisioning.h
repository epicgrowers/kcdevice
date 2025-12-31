#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "provisioning/provisioning_wifi_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start ESP-IDF BLE Wi-Fi provisioning using Security 1 (PoP required).
 */
esp_err_t idf_provisioning_start(const provisioning_wifi_ops_t *wifi_ops);

/**
 * @brief Stop provisioning service if running and reclaim BLE resources.
 */
void idf_provisioning_stop(void);

/**
 * @brief Check if provisioning service is active.
 */
bool idf_provisioning_is_running(void);

/**
 * @brief Get the BLE service name advertised during provisioning.
 */
const char *idf_provisioning_get_service_name(void);

/**
 * @brief Get the Proof-of-Possession string used for BLE provisioning.
 */
const char *idf_provisioning_get_pop(void);

#ifdef __cplusplus
}
#endif
