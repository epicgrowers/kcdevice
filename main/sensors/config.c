#include "sensors/config.h"

static const uint8_t s_ezo_addresses[] = {
    0x16, // ORP / dissolved oxygen depending on buildout
    0x63, // Dissolved oxygen / pH (Atlas factory defaults)
    0x64, // Electrical conductivity
    0x66, // RTD temperature
    0x6F  // Humidity
};

static const sensor_discovery_rules_t s_sensor_discovery_rules = {
    .ezo_addresses = s_ezo_addresses,
    .ezo_address_count = sizeof(s_ezo_addresses) / sizeof(s_ezo_addresses[0]),
    .i2c_stabilization_delay_ms = 3000,
    .sensor_init_retry_count = 5,
    .sensor_init_retry_delay_ms = 3000,
    .sensor_inter_init_delay_ms = 1500,
};

const sensor_discovery_rules_t *sensor_config_get_discovery_rules(void)
{
    return &s_sensor_discovery_rules;
}
