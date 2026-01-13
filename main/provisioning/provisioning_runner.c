#include "provisioning/provisioning_runner.h"

#include <string.h>
#include "boot/boot_config.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "provisioning/idf_provisioning.h"
#include "provisioning/provisioning_state.h"

static const char *TAG = "PROV:RUNNER";

static const provisioning_wifi_ops_t *s_wifi_ops = NULL;

static const provisioning_wifi_ops_t *provisioning_get_wifi_ops(void)
{
    if (s_wifi_ops == NULL) {
        s_wifi_ops = provisioning_wifi_ops_default();
    }
    return s_wifi_ops;
}

static char s_guard_ssid[33];
static char s_guard_password[64];
static bool s_guard_has_credentials;
static TickType_t s_last_guard_attempt = 0;

static void log_sensor_progress(const provisioning_plan_t *plan, uint32_t elapsed_ms)
{
    if (plan == NULL || plan->sensor_event_group == NULL || plan->sensor_log_interval_ms == 0) {
        return;
    }

    if ((elapsed_ms % plan->sensor_log_interval_ms) != 0) {
        return;
    }

    const EventBits_t bits = xEventGroupGetBits(plan->sensor_event_group);
    const uint32_t elapsed_seconds = elapsed_ms / 1000;

    if ((bits & plan->sensors_ready_bit) != 0) {
        ESP_LOGI(TAG, "Sensors ready (t=%lus) - waiting for provisioning", (unsigned long)elapsed_seconds);
    } else {
        ESP_LOGI(TAG, "Sensors initializing (t=%lus) - provisioning in progress", (unsigned long)elapsed_seconds);
    }

}


static void provisioning_guard_store_credentials(const char *ssid, const char *password)
{
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (ops == NULL) {
        return;
    }

    if (ssid != NULL && password != NULL) {
        strncpy(s_guard_ssid, ssid, sizeof(s_guard_ssid) - 1);
        s_guard_ssid[sizeof(s_guard_ssid) - 1] = '\0';
        strncpy(s_guard_password, password, sizeof(s_guard_password) - 1);
        s_guard_password[sizeof(s_guard_password) - 1] = '\0';
        s_guard_has_credentials = true;
        s_last_guard_attempt = 0;
        return;
    }

    char refresh_ssid[33] = {0};
    char refresh_password[64] = {0};
    if (ops->get_stored_credentials(refresh_ssid, refresh_password) == ESP_OK) {
        strncpy(s_guard_ssid, refresh_ssid, sizeof(s_guard_ssid) - 1);
        s_guard_ssid[sizeof(s_guard_ssid) - 1] = '\0';
        strncpy(s_guard_password, refresh_password, sizeof(s_guard_password) - 1);
        s_guard_password[sizeof(s_guard_password) - 1] = '\0';
        memset(refresh_password, 0, sizeof(refresh_password));
        s_guard_has_credentials = true;
        s_last_guard_attempt = 0;
    }
}

static esp_err_t attempt_stored_connection(provisioning_outcome_t *outcome)
{
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (!provisioning_wifi_ops_is_valid(ops)) {
        return ESP_ERR_INVALID_STATE;
    }

    char stored_ssid[33] = {0};
    char stored_password[64] = {0};

    if (ops->get_stored_credentials(stored_ssid, stored_password) != ESP_OK) {
        ESP_LOGI(TAG, "No stored credentials found, provisioning required");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found stored credentials, attempting to connect to: %s", stored_ssid);
    const esp_err_t ret = ops->connect(stored_ssid, stored_password);

    if (ret != ESP_OK) {
        memset(stored_password, 0, sizeof(stored_password));
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to stored WiFi network...");
    int wait_seconds = 0;
    while (!ops->is_connected() && wait_seconds < WIFI_STORED_CREDENTIAL_WAIT_SEC) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_seconds++;
    }

    if (!ops->is_connected()) {
        ESP_LOGW(TAG, "Failed to connect with stored credentials after %ds", WIFI_STORED_CREDENTIAL_WAIT_SEC);
        memset(stored_password, 0, sizeof(stored_password));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "\u2713 Connected using stored credentials");
    provisioning_state_set(PROV_STATE_PROVISIONED, STATUS_SUCCESS,
                           "Connected using stored credentials");

    provisioning_guard_store_credentials(stored_ssid, stored_password);
    memset(stored_password, 0, sizeof(stored_password));

    if (outcome != NULL) {
        outcome->connected = true;
        outcome->used_stored_credentials = true;
    }

    return ESP_OK;
}

static esp_err_t run_ble_provisioning(const provisioning_plan_t *plan, provisioning_outcome_t *outcome)
{
    if (plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (plan->on_parallel_work_start != NULL) {
        plan->on_parallel_work_start(plan->on_parallel_work_ctx);
    }

    const char *service_name = idf_provisioning_get_service_name();
    ESP_LOGI(TAG, "Starting ESP-IDF BLE provisioning (service=%s)", service_name);

    esp_err_t ret = idf_provisioning_start(provisioning_get_wifi_ops());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Waiting for provisioning (parallel tasks may continue initializing)...");

    uint32_t elapsed_ms = 0;
    while (idf_provisioning_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_ms += 1000;
        log_sensor_progress(plan, elapsed_ms);
    }

    if (idf_provisioning_is_running()) {
        idf_provisioning_stop();
    }

    ESP_LOGI(TAG, "Provisioning completed, WiFi connected");

    provisioning_guard_store_credentials(NULL, NULL);

    if (outcome != NULL) {
        outcome->connected = true;
        outcome->provisioning_triggered = true;

        if (plan->sensor_event_group != NULL) {
            const EventBits_t bits = xEventGroupGetBits(plan->sensor_event_group);
            outcome->sensors_ready_during_provisioning = ((bits & plan->sensors_ready_bit) != 0);
        }
    }

    return ESP_OK;
}

esp_err_t provisioning_run(const provisioning_plan_t *plan, provisioning_outcome_t *outcome)
{
    if (outcome != NULL) {
        memset(outcome, 0, sizeof(*outcome));
    }

    if (plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const provisioning_wifi_ops_t *ops = plan->wifi_ops;
    if (ops == NULL) {
        ops = provisioning_wifi_ops_default();
    }

    if (!provisioning_wifi_ops_is_valid(ops)) {
        ESP_LOGE(TAG, "Invalid WiFi ops table supplied to provisioning_run");
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_ops = ops;

    esp_err_t ret = ops->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return ret;
    }

    const bool attempt_stored = plan->attempt_stored_credentials_first;
    if (attempt_stored) {
        ret = attempt_stored_connection(outcome);
        if (ret == ESP_OK && outcome != NULL && outcome->connected) {
            return ESP_OK;
        }

        if (ret != ESP_ERR_NOT_FOUND && ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Stored credentials present but WiFi connect failed (%s); keeping BLE provisioning disabled while WiFi manager retries",
                     esp_err_to_name(ret));
            if (outcome != NULL) {
                outcome->waiting_for_wifi_retry = true;
            }
            return ESP_OK;
        }
    }

    ret = run_ble_provisioning(plan, outcome);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

void provisioning_connection_guard_poll(void)
{
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (ops == NULL) {
        return;
    }

    if (!s_guard_has_credentials) {
        provisioning_guard_store_credentials(NULL, NULL);
        if (!s_guard_has_credentials) {
            return;
        }
    }

    if (ops->is_connected()) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (now - s_last_guard_attempt < pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS)) {
        return;
    }

    s_last_guard_attempt = now;
    ESP_LOGW(TAG, "WiFi connection lost, attempting to reconnect to %s", s_guard_ssid);

    const esp_err_t ret = ops->connect(s_guard_ssid, s_guard_password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger WiFi reconnect: %s", esp_err_to_name(ret));
    }
}

esp_err_t provisioning_get_saved_network(provisioning_saved_network_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    char password[64] = {0};
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (ops == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ops->get_stored_credentials(info->ssid, password);
    memset(password, 0, sizeof(password));

    if (ret == ESP_OK) {
        info->has_credentials = true;
    }

    return ret;
}

esp_err_t provisioning_clear_saved_network(void)
{
    memset(s_guard_ssid, 0, sizeof(s_guard_ssid));
    memset(s_guard_password, 0, sizeof(s_guard_password));
    s_guard_has_credentials = false;
    s_last_guard_attempt = 0;
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (ops == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return ops->clear_credentials();
}

bool provisioning_wifi_is_connected(void)
{
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    return (ops != NULL) ? ops->is_connected() : false;
}

esp_err_t provisioning_disconnect(void)
{
    const provisioning_wifi_ops_t *ops = provisioning_get_wifi_ops();
    if (ops == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return ops->disconnect();
}
