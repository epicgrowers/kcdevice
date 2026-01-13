#include "boot/boot_coordinator.h"

#include "esp_log.h"

static const char *TAG = "MAIN:BOOT:COORD";

esp_err_t boot_coordinator_init(boot_coordinator_t *coordinator)
{
    if (coordinator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (coordinator->event_group != NULL) {
        return ESP_OK;
    }

    coordinator->event_group = xEventGroupCreate();
    if (coordinator->event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create boot event group");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

EventGroupHandle_t boot_coordinator_get_event_group(const boot_coordinator_t *coordinator)
{
    return (coordinator != NULL) ? coordinator->event_group : NULL;
}

static void configure_common(const boot_coordinator_t *coordinator,
                             EventBits_t ready_bit,
                             EventGroupHandle_t *event_group_out,
                             EventBits_t *ready_bit_out)
{
    if (event_group_out != NULL) {
        *event_group_out = boot_coordinator_get_event_group(coordinator);
    }

    if (ready_bit_out != NULL) {
        *ready_bit_out = ready_bit;
    }
}

void boot_coordinator_prepare_sensor_pipeline(const boot_coordinator_t *coordinator,
                                              EventBits_t ready_bit,
                                              uint32_t reading_interval_sec,
                                              sensor_pipeline_launch_ctx_t *out_ctx)
{
    if (out_ctx == NULL) {
        return;
    }

    EventGroupHandle_t group = boot_coordinator_get_event_group(coordinator);
    sensor_pipeline_prepare(out_ctx, group, ready_bit, reading_interval_sec);
}

void boot_coordinator_configure_network_boot(const boot_coordinator_t *coordinator,
                                             EventBits_t ready_bit,
                                             EventBits_t degraded_bit,
                                             network_boot_config_t *out_config)
{
    if (out_config == NULL) {
        return;
    }

    configure_common(coordinator, ready_bit, &out_config->event_group, &out_config->ready_bit);
    out_config->degraded_bit = degraded_bit;
}

EventBits_t boot_coordinator_wait_bits(const boot_coordinator_t *coordinator,
                                       EventBits_t bits,
                                       bool wait_for_all_bits,
                                       TickType_t timeout_ticks)
{
    EventGroupHandle_t group = boot_coordinator_get_event_group(coordinator);
    if (group == NULL) {
        ESP_LOGE(TAG, "Boot coordinator not initialized before wait");
        return 0;
    }

    return xEventGroupWaitBits(
        group,
        bits,
        pdFALSE,
        wait_for_all_bits ? pdTRUE : pdFALSE,
        timeout_ticks);
}

static bool is_sensor_launch_ctx_valid(const sensor_pipeline_launch_ctx_t *ctx)
{
    return ctx != NULL && ctx->event_group != NULL && ctx->ready_bit != 0;
}

esp_err_t boot_coordinator_launch_sensors_now(boot_sensor_launch_ctx_t *ctx)
{
    if (!is_sensor_launch_ctx_valid(ctx)) {
        ESP_LOGW(TAG, "Sensor launch context invalid; skipping");
        return ESP_ERR_INVALID_ARG;
    }

    return sensor_pipeline_launch(ctx);
}

void boot_coordinator_launch_sensors_async(void *ctx)
{
    sensor_pipeline_launch_async(ctx);
}
