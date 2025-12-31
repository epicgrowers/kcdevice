#pragma once

#include "freertos/FreeRTOS.h"

// Timing configuration shared across boot tasks
#define WIFI_CONNECT_TIMEOUT_MS        10000
#define WIFI_RETRY_INTERVAL_MS         30000
#define CLOUD_PROVISION_RETRY_MS       60000
#define NETWORK_READY_WAIT_MS          30000
#define PROVISIONING_SENSOR_LOG_INTERVAL_MS 10000
#define WIFI_STORED_CREDENTIAL_WAIT_SEC     30

// Task configuration
#define SENSOR_TASK_STACK_SIZE         4096
#define SENSOR_TASK_PRIORITY           5
#define NETWORK_TASK_STACK_SIZE        8192
#define NETWORK_TASK_PRIORITY          3

// Event bits for boot coordination
#define SENSORS_READY_BIT              BIT0
#define NETWORK_READY_BIT              BIT1
#define NETWORK_DEGRADED_BIT           BIT2
