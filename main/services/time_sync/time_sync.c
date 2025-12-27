/**
 * @file time_sync.c
 * @brief NTP time synchronization implementation
 */

#include "time_sync.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "TIME_SYNC";

// Default NTP servers
#define NTP_SERVER_PRIMARY   "pool.ntp.org"
#define NTP_SERVER_SECONDARY "time.nist.gov"
#define NTP_SERVER_TERTIARY  "time.google.com"

// Time sync state
static bool s_time_synced = false;
static time_sync_callback_t s_sync_callback = NULL;

/**
 * @brief Callback invoked when SNTP sync occurs
 */
static void time_sync_notification_cb(struct timeval *tv)
{
    struct tm timeinfo;
    time_t now = tv->tv_sec;
    localtime_r(&now, &timeinfo);
    
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    ESP_LOGI(TAG, "✓ Time synchronized: %s", strftime_buf);
    s_time_synced = true;
    
    // Call user callback if registered
    if (s_sync_callback != NULL) {
        s_sync_callback(true, &timeinfo);
    }
}

esp_err_t time_sync_init(const char *timezone, time_sync_callback_t callback)
{
    ESP_LOGI(TAG, "Initializing SNTP time synchronization");
    
    // Store callback
    s_sync_callback = callback;
    
    // Set timezone (default to UTC if not specified)
    if (timezone != NULL) {
        setenv("TZ", timezone, 1);
        ESP_LOGI(TAG, "Timezone set to: %s", timezone);
    } else {
        setenv("TZ", "UTC", 1);
        ESP_LOGI(TAG, "Timezone set to: UTC (default)");
    }
    tzset();
    
    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // Set primary NTP server
    esp_sntp_setservername(0, NTP_SERVER_PRIMARY);
    ESP_LOGI(TAG, "NTP server 0: %s", NTP_SERVER_PRIMARY);
    
    // Set secondary NTP server
    esp_sntp_setservername(1, NTP_SERVER_SECONDARY);
    ESP_LOGI(TAG, "NTP server 1: %s", NTP_SERVER_SECONDARY);
    
    // Set tertiary NTP server
    esp_sntp_setservername(2, NTP_SERVER_TERTIARY);
    ESP_LOGI(TAG, "NTP server 2: %s", NTP_SERVER_TERTIARY);
    
    // Set notification callback for when time is synced
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    // Start SNTP service
    esp_sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized, waiting for time sync...");
    
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_time_synced;
}

esp_err_t time_sync_get_time_string(char *buffer, size_t buffer_size, const char *format)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_time_synced) {
        ESP_LOGW(TAG, "Time not yet synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Use default ISO 8601 format if not specified
    const char *fmt = (format != NULL) ? format : "%Y-%m-%d %H:%M:%S";
    
    size_t written = strftime(buffer, buffer_size, fmt, &timeinfo);
    if (written == 0) {
        ESP_LOGE(TAG, "Failed to format time string");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t time_sync_get_timestamp(time_t *timestamp)
{
    if (timestamp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_time_synced) {
        ESP_LOGW(TAG, "Time not yet synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    
    time(timestamp);
    return ESP_OK;
}

void time_sync_deinit(void)
{
    ESP_LOGI(TAG, "Stopping SNTP service");
    esp_sntp_stop();
    s_time_synced = false;
    s_sync_callback = NULL;
}
