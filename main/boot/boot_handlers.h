#pragma once

#include "provisioning/provisioning_state.h"
#include "platform/reset_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void boot_provisioning_state_change_handler(provisioning_state_t state,
                                            provisioning_status_code_t status,
                                            const char *message);

void boot_reset_button_handler(reset_button_event_t event, uint32_t press_duration_ms);

#ifdef __cplusplus
}
#endif
