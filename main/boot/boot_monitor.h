#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Publish boot-stage telemetry when MQTT is ready.
 *
 * @param stage  Short stage identifier (e.g. "sensors_ready").
 * @param detail Optional detail string appended to the status payload.
 */
void boot_monitor_publish(const char *stage, const char *detail);

#ifdef __cplusplus
}
#endif
