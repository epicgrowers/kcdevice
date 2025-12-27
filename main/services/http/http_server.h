/**
 * @file http_server.h
 * @brief HTTPS web server for local dashboard
 * 
 * Provides secure web dashboard for device monitoring and control.
 * Uses certificates from cloud provisioning.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start HTTPS server
 * 
 * Requires valid certificates to be available (from cloud provisioning).
 * Server will be accessible at https://<device-ip>
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop HTTPS server
 * 
 * @return ESP_OK on success
 */
esp_err_t http_server_stop(void);

/**
 * @brief Check if HTTPS server is running
 * 
 * @return true if server is running
 */
bool http_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
