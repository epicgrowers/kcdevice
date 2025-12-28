/**
 * @file mdns_service.h
 * @brief mDNS service for local network discovery
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize mDNS service
 *
 * @param hostname Hostname for mDNS (e.g., "kc" for kc.local)
 * @param instance_name Instance name for service description
 * @return ESP_OK on success
 */
esp_err_t mdns_service_init(const char *hostname, const char *instance_name);

/**
 * @brief Add HTTPS service to mDNS
 *
 * @param port HTTPS port (default 443)
 * @return ESP_OK on success
 */
esp_err_t mdns_service_add_https(uint16_t port);

/**
 * @brief Stop mDNS service
 */
void mdns_service_deinit(void);

#ifdef __cplusplus
}
#endif
