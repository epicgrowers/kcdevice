#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *ezo_addresses;
    size_t ezo_address_count;
    uint32_t i2c_stabilization_delay_ms;
    uint32_t sensor_init_retry_count;
    uint32_t sensor_init_retry_delay_ms;
    uint32_t sensor_inter_init_delay_ms;
} sensor_discovery_rules_t;

const sensor_discovery_rules_t *sensor_config_get_discovery_rules(void);

#ifdef __cplusplus
}
#endif
