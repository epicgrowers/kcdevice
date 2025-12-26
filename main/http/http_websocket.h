/**
 * @file http_websocket.h
 * @brief WebSocket handlers for real-time sensor data streaming
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "sensor_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WebSocket subsystem
 * 
 * Must be called before registering handlers
 * 
 * @param server HTTPS server handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_websocket_init(httpd_handle_t server);

/**
 * @brief Deinitialize WebSocket subsystem
 * 
 * Stops all active streams and cleans up resources
 */
void http_websocket_deinit(void);

/**
 * @brief WebSocket handler for /ws/sensors endpoint
 * 
 * Handles sensor data streaming via WebSocket
 * 
 * @param req HTTP request
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sensor_ws_handler(httpd_req_t *req);

/**
 * @brief Callback for sensor cache updates
 * 
 * Called by sensor_manager when new data is available
 * Broadcasts updates to all connected WebSocket clients
 * 
 * @param cache Sensor cache data
 * @param ctx User context (unused)
 */
void http_websocket_cache_update_handler(const sensor_cache_t *cache, void *ctx);

#ifdef __cplusplus
}
#endif
