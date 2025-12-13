#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "chip_info.h"
#include "idf_provisioning.h"
#include "wifi_manager.h"
#include "provisioning_state.h"
#include "security.h"
#include "reset_button.h"
#include "time_sync.h"
#include "cloud_provisioning.h"
#include "http_server.h"
#include "api_key_manager.h"
#include "mdns_service.h"
#include "mqtt_telemetry.h"
#include "i2c_scanner.h"
#include "sensor_manager.h"

static const char *TAG = "MAIN";

// ============================================================================
// Parallel Boot Configuration
// ============================================================================

// Timing Configuration
#define I2C_STABILIZATION_DELAY_MS     3000   // Initial wait for sensor power-up
#define SENSOR_INIT_RETRY_COUNT        5      // Attempts per sensor
#define SENSOR_INIT_RETRY_DELAY_MS     3000   // Between retry attempts (3 seconds)
#define SENSOR_INTER_INIT_DELAY_MS     1500   // Between different sensors (1.5 seconds)
#define WIFI_CONNECT_TIMEOUT_MS        10000  // WiFi connection timeout (10 seconds)
#define WIFI_RETRY_INTERVAL_MS         30000  // WiFi retry interval (30 seconds)
#define CLOUD_PROVISION_RETRY_MS       60000  // Cloud provision retry (60 seconds)

// Task Configuration
#define SENSOR_TASK_STACK_SIZE         4096   // 4KB stack
#define SENSOR_TASK_PRIORITY           5      // Higher priority - user facing
#define NETWORK_TASK_STACK_SIZE        8192   // 8KB stack - TLS needs more
#define NETWORK_TASK_PRIORITY          3      // Lower priority - background

// Event Group Bits for Task Synchronization
#define SENSORS_READY_BIT              BIT0   // Sensor task completed
#define NETWORK_READY_BIT              BIT1   // Network task completed

// Global event group for task synchronization
static EventGroupHandle_t s_boot_event_group = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

// Boot tasks
static void sensor_task(void *arg);
static void network_task(void *arg);

// Legacy provisioning handlers
static void state_change_handler(provisioning_state_t state, provisioning_status_code_t status, const char* message);
static void reset_button_handler(reset_button_event_t event, uint32_t press_duration_ms);
static void time_sync_handler(bool synced, struct tm *current_time);
static void cloud_prov_handler(bool success, const char *message);

// Legacy function (will be refactored into tasks)
static void start_cloud_services(void);

/**
 * @brief Main application entry point
 * Implements parallel boot architecture with sensor and network tasks
 */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "KannaCloud Device - Parallel Boot v2.0");
    ESP_LOGI(TAG, "========================================");
    
    // Enable verbose logging for provisioning components
    esp_log_level_set("wifi_prov_mgr", ESP_LOG_DEBUG);
    esp_log_level_set("protocomm", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_prov_scheme_ble", ESP_LOG_DEBUG);
    
    // Log chip information
    chip_info_log();
    
    // ========================================================================
    // Phase 1: Basic Hardware Initialization
    // ========================================================================
    ESP_LOGI(TAG, "MAIN: Initializing basic hardware...");
    
    // Initialize security features (NVS encryption with eFuse protection)
    esp_err_t ret = security_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Security initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "MAIN: Device will continue but credentials may not be secure!");
    }
    
    // Initialize reset button (GPIO0 - BOOT button)
    ret = reset_button_init(RESET_BUTTON_GPIO, reset_button_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Failed to initialize reset button: %s", esp_err_to_name(ret));
    }
    
    // Initialize I2C bus hardware (required for sensors)
    ESP_LOGI(TAG, "MAIN: Initializing I2C bus...");
    ret = i2c_scanner_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: I2C bus initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "MAIN: Restarting device (hardware issue)...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    // Initialize provisioning state machine
    provisioning_state_init();
    provisioning_state_register_callback(state_change_handler);
    
    // Create event group for task synchronization
    s_boot_event_group = xEventGroupCreate();
    if (s_boot_event_group == NULL) {
        ESP_LOGE(TAG, "MAIN: Failed to create event group");
        esp_restart();
    }
    
    // ========================================================================
    // Phase 2: WiFi Connection (Required for Network Services)
    // ========================================================================
    bool connected = false;
    bool sensor_task_launched = false;  // Track if sensor task was launched during provisioning
    char stored_ssid[33] = {0};
    char stored_password[64] = {0};

    // Initialize WiFi
    ESP_LOGI(TAG, "MAIN: Initializing WiFi manager...");
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Failed to initialize WiFi manager");
        return;
    }

    // Check for stored credentials
    if (wifi_manager_get_stored_credentials(stored_ssid, stored_password) == ESP_OK) {
        ESP_LOGI(TAG, "MAIN: Found stored credentials, attempting to connect to: %s", stored_ssid);

        ret = wifi_manager_connect(stored_ssid, stored_password);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "MAIN: Connecting to stored WiFi network...");

            int wait_time = 0;
            while (!wifi_manager_is_connected() && wait_time < 30) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                wait_time++;
            }

            if (wifi_manager_is_connected()) {
                ESP_LOGI(TAG, "MAIN: ✓ Connected using stored credentials");
                provisioning_state_set(PROV_STATE_PROVISIONED, STATUS_SUCCESS,
                                       "Connected using stored credentials");
                connected = true;
            } else {
                ESP_LOGW(TAG, "MAIN: Failed to connect with stored credentials");
            }
        }

        memset(stored_password, 0, sizeof(stored_password));
    } else {
        ESP_LOGI(TAG, "MAIN: No stored credentials found, starting provisioning");
    }

    // Start BLE provisioning if not connected
    if (!connected) {
        const char *service_name = idf_provisioning_get_service_name();
        ESP_LOGI(TAG, "MAIN: Starting ESP-IDF BLE provisioning (service=%s)", service_name);

        // ⭐ NEW: Launch sensor task BEFORE provisioning to utilize wait time
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "MAIN: Launching sensor task during provisioning");
        ESP_LOGI(TAG, "========================================");
        
        BaseType_t task_ret = xTaskCreate(
            sensor_task,
            "sensor_task",
            SENSOR_TASK_STACK_SIZE,
            NULL,
            SENSOR_TASK_PRIORITY,
            NULL
        );
        
        if (task_ret != pdPASS) {
            ESP_LOGW(TAG, "MAIN: Failed to create sensor task during provisioning");
        } else {
            ESP_LOGI(TAG, "MAIN: ✓ Sensor task launched (priority %d)", SENSOR_TASK_PRIORITY);
            sensor_task_launched = true;
        }

        ret = idf_provisioning_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MAIN: Failed to start provisioning: %s", esp_err_to_name(ret));
            return;
        }

        // Wait for provisioning to complete (sensors initializing in parallel)
        ESP_LOGI(TAG, "MAIN: Waiting for provisioning (sensors initializing in background)...");
        int provisioning_wait_seconds = 0;
        while (idf_provisioning_is_running()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            provisioning_wait_seconds++;
            
            // Check and log sensor progress every 10 seconds
            if (provisioning_wait_seconds % 10 == 0) {
                EventBits_t bits = xEventGroupGetBits(s_boot_event_group);
                if (bits & SENSORS_READY_BIT) {
                    ESP_LOGI(TAG, "MAIN: ✓ Sensors ready (t=%ds) - waiting for provisioning", 
                             provisioning_wait_seconds);
                } else {
                    ESP_LOGI(TAG, "MAIN: Sensors initializing (t=%ds) - provisioning in progress", 
                             provisioning_wait_seconds);
                }
            }
        }

        ESP_LOGI(TAG, "MAIN: Provisioning completed, WiFi connected");
        if (idf_provisioning_is_running()) {
            idf_provisioning_stop();
        }

        // Check if sensors completed during provisioning
        EventBits_t bits = xEventGroupGetBits(s_boot_event_group);
        if (bits & SENSORS_READY_BIT) {
            ESP_LOGI(TAG, "MAIN: ✓ Sensors initialized during provisioning! (data already available)");
        } else {
            ESP_LOGI(TAG, "MAIN: Sensors still initializing...");
        }

        connected = true;
    }
    
    // ========================================================================
    // Phase 3: Launch Network Task (+ sensors if not already launched)
    // ========================================================================
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "MAIN: Launching network services");
    ESP_LOGI(TAG, "========================================");
    
    // Launch sensor task if not already launched during provisioning
    // (This handles the case where credentials existed and provisioning was skipped)
    if (!sensor_task_launched) {
        ESP_LOGI(TAG, "MAIN: Launching sensor task now...");
        BaseType_t task_ret = xTaskCreate(
            sensor_task,
            "sensor_task",
            SENSOR_TASK_STACK_SIZE,
            NULL,
            SENSOR_TASK_PRIORITY,
            NULL
        );
        
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "MAIN: Failed to create sensor task");
        } else {
            ESP_LOGI(TAG, "MAIN: ✓ Sensor task launched (priority %d)", SENSOR_TASK_PRIORITY);
        }
    }
    
    // Launch network task (lower priority - background)
    BaseType_t task_ret = xTaskCreate(
        network_task,
        "network_task",
        NETWORK_TASK_STACK_SIZE,
        NULL,
        NETWORK_TASK_PRIORITY,
        NULL
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "MAIN: Failed to create network task");
    } else {
        ESP_LOGI(TAG, "MAIN: ✓ Network task launched (priority %d)", NETWORK_TASK_PRIORITY);
    }
    
    // ========================================================================
    // Phase 4: Wait for Boot Tasks to Complete
    // ========================================================================
    ESP_LOGI(TAG, "MAIN: Waiting for sensor task to complete...");
    EventBits_t bits = xEventGroupWaitBits(
        s_boot_event_group,
        SENSORS_READY_BIT,
        pdFALSE,  // Don't clear bits
        pdFALSE,  // Wait for any bit (not all)
        pdMS_TO_TICKS(60000)  // 60 second timeout
    );
    
    if (bits & SENSORS_READY_BIT) {
        ESP_LOGI(TAG, "MAIN: ✓ Sensors ready");
    } else {
        ESP_LOGW(TAG, "MAIN: Sensor task timeout (60s), continuing anyway");
    }
    
    // Network task is optional - don't wait, just check status
    bits = xEventGroupGetBits(s_boot_event_group);
    if (bits & NETWORK_READY_BIT) {
        ESP_LOGI(TAG, "MAIN: ✓ Network services ready");
    } else {
        ESP_LOGI(TAG, "MAIN: Network services still initializing (running in background)");
    }
    
    // ========================================================================
    // Phase 5: Enter Normal Operation Mode
    // ========================================================================
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "MAIN: ✓ Boot complete, entering normal operation");
    ESP_LOGI(TAG, "========================================");
    
    // Store credentials for reconnection
    static char stored_reconnect_ssid[33] = {0};
    static char stored_reconnect_password[64] = {0};
    bool has_stored_creds = (wifi_manager_get_stored_credentials(stored_reconnect_ssid, stored_reconnect_password) == ESP_OK);
    
    // Main loop - monitor WiFi connection
    while (1) {
        if (!wifi_manager_is_connected() && has_stored_creds) {
            ESP_LOGW(TAG, "MAIN: WiFi connection lost, attempting to reconnect to %s", stored_reconnect_ssid);
            wifi_manager_connect(stored_reconnect_ssid, stored_reconnect_password);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ============================================================================
// Sensor Task - Parallel Sensor Initialization
// ============================================================================

/**
 * @brief Sensor initialization task with retry logic
 * Runs in parallel with network task, higher priority for user-facing operation
 */
static void sensor_task(void *arg)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SENSOR TASK: Starting");
    ESP_LOGI(TAG, "========================================");
    
    esp_err_t ret;
    
    // Phase 1: I2C Stabilization (3 seconds)
    ESP_LOGI(TAG, "SENSOR: Waiting %dms for I2C sensor stabilization...", I2C_STABILIZATION_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(I2C_STABILIZATION_DELAY_MS));
    
    // Phase 2: Initial I2C Scan
    ESP_LOGI(TAG, "SENSOR: Performing initial I2C scan...");
    ret = i2c_scanner_scan();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SENSOR: I2C scan encountered errors: %s", esp_err_to_name(ret));
    }
    
    // Phase 3: Sensor Initialization with Retry Logic
    ESP_LOGI(TAG, "SENSOR: Initializing sensor manager...");
    
    // The sensor_manager_init() already has retry logic built in
    // We'll call it multiple times if it fails completely
    int init_attempts = 0;
    while (init_attempts < SENSOR_INIT_RETRY_COUNT) {
        ret = sensor_manager_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SENSOR: ✓ Sensor manager initialized successfully");
            break;
        }
        
        init_attempts++;
        if (init_attempts < SENSOR_INIT_RETRY_COUNT) {
            ESP_LOGW(TAG, "SENSOR: Init attempt %d/%d failed, retrying in %dms...", 
                     init_attempts, SENSOR_INIT_RETRY_COUNT, SENSOR_INIT_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(SENSOR_INIT_RETRY_DELAY_MS));
        } else {
            ESP_LOGW(TAG, "SENSOR: Failed to initialize sensor manager after %d attempts", 
                     SENSOR_INIT_RETRY_COUNT);
            ESP_LOGW(TAG, "SENSOR: Continuing with no sensors (graceful degradation)");
        }
    }
    
    // Phase 4: Final Verification Scan
    ESP_LOGI(TAG, "SENSOR: Performing final I2C verification scan...");
    i2c_scanner_scan();
    
    // Log comprehensive sensor inventory
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SENSOR INVENTORY:");
    ESP_LOGI(TAG, "  Battery Monitor: %s", sensor_manager_has_battery_monitor() ? "YES" : "NO");
    ESP_LOGI(TAG, "  EZO Sensors: %d", sensor_manager_get_ezo_count());
    ESP_LOGI(TAG, "========================================");
    
    // Phase 5: Start Sensor Reading Task
    ESP_LOGI(TAG, "SENSOR: Starting sensor reading task...");
    ret = sensor_manager_start_reading_task(10);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SENSOR: Failed to start reading task: %s", esp_err_to_name(ret));
    }
    
    // Phase 6: Signal Sensor Task Ready
    ESP_LOGI(TAG, "SENSOR: ✓ Sensor task complete, signaling SENSORS_READY");
    xEventGroupSetBits(s_boot_event_group, SENSORS_READY_BIT);
    
    // Task complete - delete itself
    vTaskDelete(NULL);
}

// ============================================================================
// Network Task - Parallel Network Initialization
// ============================================================================

/**
 * @brief Network initialization task with proper TLS ordering
 * Runs in parallel with sensor task, lower priority for background operation
 */
static void network_task(void *arg)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK TASK: Starting");
    ESP_LOGI(TAG, "========================================");
    
    esp_err_t ret;
    
    // WiFi should already be initialized and connected by this point from app_main
    // But we'll verify and wait if needed
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "NETWORK: WiFi not connected, waiting...");
        int wait_time = 0;
        while (!wifi_manager_is_connected() && wait_time < (WIFI_CONNECT_TIMEOUT_MS / 1000)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_time++;
        }
        
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "NETWORK: WiFi connection timeout, network services unavailable");
            // Don't signal NETWORK_READY - continue with sensors only
            vTaskDelete(NULL);
            return;
        }
    }
    
    ESP_LOGI(TAG, "NETWORK: ✓ WiFi connected");
    
    // Phase 1: Time Synchronization (required for TLS certificate validation)
    ESP_LOGI(TAG, "NETWORK: Initializing NTP time synchronization...");
    ret = time_sync_init(NULL, time_sync_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NETWORK: Failed to initialize time sync: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "NETWORK: Waiting for time synchronization...");
    int sync_wait = 0;
    while (!time_sync_is_synced() && sync_wait < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sync_wait++;
    }
    
    if (!time_sync_is_synced()) {
        ESP_LOGW(TAG, "NETWORK: Time sync timeout, TLS may fail");
    }
    
    // Phase 2: Cloud Provisioning (CRITICAL - must complete before TLS services)
    ESP_LOGI(TAG, "NETWORK: Initializing API key manager...");
    api_key_manager_init();
    
    ESP_LOGI(TAG, "NETWORK: Initializing cloud provisioning...");
    cloud_prov_init(cloud_prov_handler);
    
    ESP_LOGI(TAG, "NETWORK: Starting cloud provisioning (fetching certificates)...");
    ret = cloud_prov_provision_device();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NETWORK: Cloud provisioning failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "NETWORK: Retrying cloud provisioning in %dms...", CLOUD_PROVISION_RETRY_MS);
        
        // Retry cloud provisioning with longer timeout
        vTaskDelay(pdMS_TO_TICKS(CLOUD_PROVISION_RETRY_MS));
        ret = cloud_prov_provision_device();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NETWORK: Cloud provisioning failed after retry, network services unavailable");
            vTaskDelete(NULL);
            return;
        }
    }
    
    ESP_LOGI(TAG, "NETWORK: ✓ Cloud provisioning complete (certificates obtained)");
    
    // Download MQTT CA certificate
    ESP_LOGI(TAG, "NETWORK: Downloading MQTT CA certificate...");
    ret = cloud_prov_download_mqtt_ca_cert();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NETWORK: Failed to download MQTT CA cert: %s", esp_err_to_name(ret));
    }
    
    // Phase 3: Start MQTT Client (requires certificates)
    ESP_LOGI(TAG, "NETWORK: Initializing MQTT client...");
    const char *mqtt_broker = "mqtts://mqtt.kannacloud.com:8883";
    const char *mqtt_username = "sensor01";
    const char *mqtt_password = "xkKKYQWxiT83Ni3";
    
    ret = mqtt_client_init(mqtt_broker, mqtt_username, mqtt_password);
    if (ret == ESP_OK) {
        ret = mqtt_client_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "NETWORK: ✓ MQTT client started");
        } else {
            ESP_LOGW(TAG, "NETWORK: Failed to start MQTT: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "NETWORK: Failed to init MQTT: %s", esp_err_to_name(ret));
    }
    
#ifndef CONFIG_IDF_TARGET_ESP32C6
    // Phase 4: Start HTTPS Server (S3 only, requires certificates)
    ESP_LOGI(TAG, "NETWORK: Initializing mDNS service...");
    ret = mdns_service_init("kc", "KannaCloud Device");
    if (ret == ESP_OK) {
        mdns_service_add_https(443);
    } else {
        ESP_LOGW(TAG, "NETWORK: mDNS init failed: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "NETWORK: Starting HTTPS dashboard server...");
    ret = http_server_start();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NETWORK: ✓ HTTPS server started");
        ESP_LOGI(TAG, "NETWORK: ✓ Access dashboard at: https://kc.local");
    } else {
        ESP_LOGE(TAG, "NETWORK: Failed to start HTTPS server: %s", esp_err_to_name(ret));
    }
#else
    ESP_LOGI(TAG, "NETWORK: Running in cloud-only mode (ESP32-C6)");
#endif
    
    // Phase 5: Signal Network Task Ready
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK: ✓ All network services started");
    ESP_LOGI(TAG, "========================================");
    xEventGroupSetBits(s_boot_event_group, NETWORK_READY_BIT);
    
    // Task complete - delete itself
    vTaskDelete(NULL);
}

/**
 * @brief Start cloud services (LEGACY - kept for compatibility)
 * This function is now replaced by network_task, but kept to avoid breaking existing code paths
 */
static void start_cloud_services(void)
{
    esp_err_t ret;
    
    // Initialize NTP time synchronization
    ESP_LOGI(TAG, "Initializing NTP time synchronization...");
    ret = time_sync_init(NULL, time_sync_handler); // NULL = UTC timezone
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize time sync: %s", esp_err_to_name(ret));
    }
    
    // Wait for time sync (required for HTTPS certificate validation)
    ESP_LOGI(TAG, "Waiting for time synchronization...");
    int sync_wait = 0;
    while (!time_sync_is_synced() && sync_wait < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sync_wait++;
    }
    
    // Initialize API key manager
    ESP_LOGI(TAG, "Initializing API key manager...");
    api_key_manager_init();
    
    // Initialize cloud provisioning
    ESP_LOGI(TAG, "Initializing cloud provisioning...");
    cloud_prov_init(cloud_prov_handler);
    
    // Start automatic provisioning (get certificates)
    ESP_LOGI(TAG, "Starting cloud provisioning...");
    ret = cloud_prov_provision_device();
    if (ret == ESP_OK) {
        // Download MQTT CA certificate for MQTTS
        ESP_LOGI(TAG, "Downloading MQTT CA certificate...");
        ret = cloud_prov_download_mqtt_ca_cert();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to download MQTT CA certificate: %s", esp_err_to_name(ret));
        }
        
#ifndef CONFIG_IDF_TARGET_ESP32C6
        // ESP32-S3: Full local dashboard support
        // Initialize mDNS for local network discovery
        ESP_LOGI(TAG, "Initializing mDNS service...");
        ret = mdns_service_init("kc", "KannaCloud Device");
        if (ret == ESP_OK) {
            mdns_service_add_https(443);
        } else {
            ESP_LOGW(TAG, "mDNS initialization failed, device accessible by IP only");
        }
        
        // Start HTTPS server with downloaded certificates
        ESP_LOGI(TAG, "Starting HTTPS dashboard server...");
        ret = http_server_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ HTTPS dashboard is ready!");
            ESP_LOGI(TAG, "✓ Access at: https://kc.local");
        } else {
            ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(ret));
        }
#else
        // ESP32-C6: Cloud-only mode (no local dashboard)
        ESP_LOGI(TAG, "Running in cloud-only mode (ESP32-C6 - no local dashboard)");
#endif
        
        // Initialize and scan I2C bus for sensors
        ESP_LOGI(TAG, "Initializing I2C scanner...");
        ret = i2c_scanner_init();
        if (ret == ESP_OK) {
            i2c_scanner_scan();
            
            // Initialize sensor manager for real sensor data
            ESP_LOGI(TAG, "Initializing sensor manager...");
            ret = sensor_manager_init();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✓ Sensors initialized: Battery=%s, EZO sensors=%d",
                         sensor_manager_has_battery_monitor() ? "YES" : "NO",
                         sensor_manager_get_ezo_count());
            } else {
                ESP_LOGW(TAG, "Failed to initialize sensors: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize I2C: %s", esp_err_to_name(ret));
        }
        
        // Start sensor reading task (10 second interval)
        ESP_LOGI(TAG, "Starting sensor reading task...");
        ret = sensor_manager_start_reading_task(10);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start sensor reading task: %s", esp_err_to_name(ret));
        }
        
        // Initialize and start MQTT client for KannaCloud telemetry
        ESP_LOGI(TAG, "Initializing MQTT client...");
        const char *mqtt_broker = "mqtts://mqtt.kannacloud.com:8883";
        const char *mqtt_username = "sensor01";
        const char *mqtt_password = "xkKKYQWxiT83Ni3";
        ret = mqtt_client_init(mqtt_broker, mqtt_username, mqtt_password);
        if (ret == ESP_OK) {
            ret = mqtt_client_start();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✓ MQTT telemetry enabled");
                // MQTT interval already loaded from NVS in mqtt_client_init()
            } else {
                ESP_LOGW(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize MQTT client: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Cloud provisioning failed, dashboard not available");
    }
}

/**
 * @brief Handle state changes and send BLE notifications
 */
static void state_change_handler(provisioning_state_t state, provisioning_status_code_t status, const char* message)
{
    ESP_LOGI(TAG, "State changed: %s | Status: %s | Message: %s",
             provisioning_state_to_string(state),
             provisioning_status_to_string(status),
             message ? message : "N/A");
}

/**
 * @brief Handle reset button events
 */
static void reset_button_handler(reset_button_event_t event, uint32_t press_duration_ms)
{
    switch (event) {
        case RESET_BUTTON_EVENT_SHORT_PRESS:
            ESP_LOGW(TAG, "====================================");
            ESP_LOGW(TAG, "SHORT PRESS DETECTED (%lu ms)", (unsigned long)press_duration_ms);
            ESP_LOGW(TAG, "Clearing WiFi credentials...");
            ESP_LOGW(TAG, "====================================");
            
            // Clear WiFi credentials
            esp_err_t ret = wifi_manager_clear_credentials();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "WiFi credentials cleared successfully");
                ESP_LOGI(TAG, "Restarting device to begin reprovisioning...");
                
                // Disconnect WiFi first
                wifi_manager_disconnect();
                
                // Wait a moment then restart
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Failed to clear credentials: %s", esp_err_to_name(ret));
            }
            break;
            
        case RESET_BUTTON_EVENT_LONG_PRESS:
            ESP_LOGW(TAG, "====================================");
            ESP_LOGW(TAG, "LONG PRESS DETECTED (%lu ms)", (unsigned long)press_duration_ms);
            ESP_LOGW(TAG, "Performing FACTORY RESET...");
            ESP_LOGW(TAG, "====================================");
            
            // Erase entire NVS partition (factory reset)
            ret = nvs_flash_erase();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "NVS erased successfully (factory reset)");
                ESP_LOGI(TAG, "Restarting device...");
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
            }
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown button event: %d", event);
            break;
    }
}

/**
 * @brief Handle time synchronization events
 */
static void time_sync_handler(bool synced, struct tm *current_time)
{
    if (synced && current_time != NULL) {
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", current_time);
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Time Synchronized Successfully!");
        ESP_LOGI(TAG, "Current time: %s UTC", time_str);
        ESP_LOGI(TAG, "====================================");
    } else {
        ESP_LOGW(TAG, "Time synchronization failed");
    }
}

/**
 * @brief Handle cloud provisioning events
 */
static void cloud_prov_handler(bool success, const char *message)
{
    if (success) {
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Cloud Provisioning Successful!");
        ESP_LOGI(TAG, "Message: %s", message ? message : "N/A");
        ESP_LOGI(TAG, "====================================");
    } else {
        ESP_LOGW(TAG, "====================================");
        ESP_LOGW(TAG, "Cloud Provisioning Failed");
        ESP_LOGW(TAG, "Error: %s", message ? message : "Unknown");
        ESP_LOGW(TAG, "====================================");
    }
}
