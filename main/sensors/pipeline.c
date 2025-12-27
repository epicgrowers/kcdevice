#include "sensors/pipeline.h"

#include <string.h>
#include "esp_log.h"
#include "sensors/sensor_boot.h"

typedef struct {
    sensor_pipeline_snapshot_listener_t listener;
    void *listener_ctx;
    sensor_pipeline_launch_ctx_t *pipeline_ctx;
} sensor_pipeline_listener_state_t;

static const char *TAG = "SENSOR_PIPELINE";
static sensor_pipeline_launch_ctx_t *s_default_ctx = NULL;
static sensor_pipeline_listener_state_t s_listener_state = {0};

static sensor_pipeline_launch_ctx_t *resolve_ctx(sensor_pipeline_launch_ctx_t *ctx)
{
    if (ctx != NULL) {
        return ctx;
    }
    return s_default_ctx;
}

static const sensor_pipeline_launch_ctx_t *resolve_ctx_const(const sensor_pipeline_launch_ctx_t *ctx)
{
    if (ctx != NULL) {
        return ctx;
    }
    return s_default_ctx;
}

static void populate_readiness(const sensor_pipeline_launch_ctx_t *ctx,
                               sensor_pipeline_readiness_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (ctx == NULL) {
        return;
    }

    out->event_group = ctx->event_group;
    out->ready_bit = ctx->ready_bit;
    out->sensor_task_launched = ctx->sensor_task_launched;

    if (ctx->event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(ctx->event_group);
        out->raw_event_bits = bits;
        if (ctx->ready_bit != 0) {
            out->sensors_ready = (bits & ctx->ready_bit) == ctx->ready_bit;
        }
    }
}

static void sensor_pipeline_cache_listener(const sensor_cache_t *cache, void *user_ctx)
{
    sensor_pipeline_listener_state_t *state = (sensor_pipeline_listener_state_t *)user_ctx;
    if (state == NULL || state->listener == NULL) {
        return;
    }

    sensor_pipeline_launch_ctx_t *ctx = resolve_ctx(state->pipeline_ctx);
    if (ctx == NULL) {
        return;
    }

    sensor_pipeline_snapshot_t snapshot = {0};
    populate_readiness(ctx, &snapshot.readiness);
    snapshot.reading_interval_sec = ctx->reading_interval_sec;
    if (cache != NULL) {
        snapshot.cache = *cache;
        snapshot.cache_valid = true;
    }

    state->listener(&snapshot, state->listener_ctx);
}

static uint32_t normalize_interval(uint32_t interval)
{
    if (interval == 0) {
        return SENSOR_PIPELINE_DEFAULT_INTERVAL_SEC;
    }
    return interval;
}

void sensor_pipeline_prepare(sensor_pipeline_launch_ctx_t *ctx,
                             EventGroupHandle_t event_group,
                             EventBits_t ready_bit,
                             uint32_t reading_interval_sec)
{
    if (ctx == NULL) {
        return;
    }

    ctx->event_group = event_group;
    ctx->ready_bit = ready_bit;
    ctx->reading_interval_sec = normalize_interval(reading_interval_sec);
    ctx->sensor_task_launched = false;

    s_default_ctx = ctx;
}

const sensor_pipeline_launch_ctx_t *sensor_pipeline_get_active_ctx(void)
{
    return s_default_ctx;
}

esp_err_t sensor_pipeline_launch(sensor_pipeline_launch_ctx_t *ctx)
{
    ctx = resolve_ctx(ctx);
    if (ctx == NULL || ctx->event_group == NULL || ctx->ready_bit == 0) {
        ESP_LOGW(TAG, "Invalid sensor pipeline context; skipping launch");
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->sensor_task_launched) {
        ESP_LOGD(TAG, "Sensor pipeline already launched");
        return ESP_OK;
    }

    sensor_boot_config_t boot_cfg = {
        .event_group = ctx->event_group,
        .ready_bit = ctx->ready_bit,
        .reading_interval_sec = ctx->reading_interval_sec,
    };

    esp_err_t ret = sensor_boot_start(&boot_cfg);
    if (ret == ESP_OK) {
        ctx->sensor_task_launched = true;
        ESP_LOGI(TAG, "Sensor pipeline launch requested");
    } else {
        ESP_LOGW(TAG, "Sensor pipeline launch failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

void sensor_pipeline_launch_async(void *ctx)
{
    esp_err_t ret = sensor_pipeline_launch((sensor_pipeline_launch_ctx_t *)ctx);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async sensor pipeline launch failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t sensor_pipeline_snapshot(const sensor_pipeline_launch_ctx_t *ctx,
                                   sensor_pipeline_snapshot_t *out_snapshot)
{
    const sensor_pipeline_launch_ctx_t *resolved = resolve_ctx_const(ctx);
    if (resolved == NULL || out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    populate_readiness(resolved, &out_snapshot->readiness);
    out_snapshot->reading_interval_sec = resolved->reading_interval_sec;

    sensor_cache_t cache = {0};
    esp_err_t cache_ret = sensor_manager_get_cached_data(&cache);
    if (cache_ret == ESP_OK) {
        out_snapshot->cache = cache;
        out_snapshot->cache_valid = true;
    }

    return ESP_OK;
}

esp_err_t sensor_pipeline_register_snapshot_listener(sensor_pipeline_launch_ctx_t *ctx,
                                                     sensor_pipeline_snapshot_listener_t listener,
                                                     void *user_ctx)
{
    if (listener == NULL) {
        sensor_manager_register_cache_listener(NULL, NULL);
        memset(&s_listener_state, 0, sizeof(s_listener_state));
        return ESP_OK;
    }

    sensor_pipeline_launch_ctx_t *resolved = resolve_ctx(ctx);
    if (resolved == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_listener_state.listener = listener;
    s_listener_state.listener_ctx = user_ctx;
    s_listener_state.pipeline_ctx = resolved;

    sensor_manager_register_cache_listener(sensor_pipeline_cache_listener, &s_listener_state);
    return ESP_OK;
}
