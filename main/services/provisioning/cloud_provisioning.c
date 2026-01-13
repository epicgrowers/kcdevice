/**
 * @file cloud_provisioning.c
 * @brief Cloud provisioning implementation
 */

#include "cloud_provisioning.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "services/keys/api_key_manager.h"
#include <string.h>

static const char *TAG = "SERVICES:CLOUD_PROV";

// NVS namespace and partition for certificates
#ifdef CONFIG_IDF_TARGET_ESP32C6
#define NVS_PARTITION "nvs"  // Use main NVS partition on C6 (no dedicated cert partition)
#else
#define NVS_PARTITION "nvs_certs"  // Dedicated partition on S3
#endif
#define NVS_NAMESPACE "certs"
#define NVS_KEY_CERT "device_cert"
#define NVS_KEY_PRIVATE "device_key"
#define NVS_KEY_CERT_ID "cert_id"
#define NVS_KEY_MQTT_CA "mqtt_ca_cert"

// Device name stored in settings namespace (main nvs partition)
#define NVS_SETTINGS_NAMESPACE "settings"
#define NVS_KEY_DEVICE_NAME "device_name"

// Callback
static cloud_prov_callback_t s_callback = NULL;

// Buffers for HTTP responses
static char s_response_buffer[4096];
static size_t s_response_len = 0;

/**
 * @brief HTTP event handler
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (s_response_len + evt->data_len < sizeof(s_response_buffer)) {
                memcpy(s_response_buffer + s_response_len, evt->data, evt->data_len);
                s_response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Get MAC address as device ID
 */
static void get_device_mac_string(char *mac_str, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t attach_cloud_api_key_header(esp_http_client_handle_t client)
{
    api_key_t cloud_key;
    esp_err_t err = api_key_manager_get_by_type(API_KEY_TYPE_CLOUD_SERVER, &cloud_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cloud API key unavailable: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "Update config/runtime/api_keys.json and reboot to seed the secret");
        return err;
    }

    if (cloud_key.key[0] == '\0') {
        ESP_LOGE(TAG, "Cloud API key is empty; update config/runtime/api_keys.json");
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_http_client_set_header(client, "X-API-Key", cloud_key.key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set API key header: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t cloud_prov_init(cloud_prov_callback_t callback)
{
    ESP_LOGI(TAG, "Initializing cloud provisioning");
    s_callback = callback;
    return ESP_OK;
}

esp_err_t cloud_prov_get_device_id(char *id_out, size_t id_len)
{
    if (id_out == NULL || id_len < 20) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Device ID is always MAC-based and immutable
    char mac_str[13];
    get_device_mac_string(mac_str, sizeof(mac_str));
    snprintf(id_out, id_len, "kc-%s", mac_str);
    
    return ESP_OK;
}

esp_err_t cloud_prov_get_device_name(char *name_out, size_t name_len)
{
    if (name_out == NULL || name_len < 65) {
        return ESP_ERR_INVALID_ARG;
    }
    
    name_out[0] = '\0'; // Default to empty string
    
    // Try to load custom device name from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK || err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err == ESP_OK) {
            size_t required_size = name_len;
            err = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_NAME, name_out, &required_size);
            nvs_close(nvs_handle);
            
            if (err == ESP_OK && name_out[0] != '\0') {
                ESP_LOGI(TAG, "Using custom device name: %s", name_out);
                return ESP_OK;
            }
        }
    }
    
    return ESP_OK;
}

esp_err_t cloud_prov_set_device_name(const char *device_name)
{
    if (device_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate device name (1-64 chars)
    size_t len = strlen(device_name);
    if (len < 1 || len > 64) {
        ESP_LOGE(TAG, "Device name must be 1-64 characters");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize main NVS partition
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Main NVS needs erase, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize main NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // Open settings namespace
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", 
                 NVS_SETTINGS_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    
    // Save device name
    err = nvs_set_str(nvs_handle, NVS_KEY_DEVICE_NAME, device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write device name to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit device name to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    ESP_LOGI(TAG, "Device name saved: %s", device_name);
    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t cloud_prov_clear_device_name(void)
{
    ESP_LOGI(TAG, "Clearing device name");
    
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }
    
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open settings namespace: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, NVS_KEY_DEVICE_NAME);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(nvs_handle);
        err = ESP_OK;
    }
    
    nvs_close(nvs_handle);
    return err;
}

bool cloud_prov_has_certificates(void)
{
    // Initialize partition if not already done
    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition needs to be erased - no certificates anyway
        return false;
    }
    
    nvs_handle_t nvs_handle;
    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }
    
    // Check if certificate exists
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_CERT, NULL, &required_size);
    nvs_close(nvs_handle);
    
    return (err == ESP_OK && required_size > 0);
}

esp_err_t cloud_prov_get_certificate(char *cert_out, size_t *cert_len)
{
    if (cert_out == NULL || cert_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t required_size = CLOUD_PROV_MAX_CERT_SIZE;
    err = nvs_get_str(nvs_handle, NVS_KEY_CERT, cert_out, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        // nvs_get_str returns size including null terminator
        // Return the actual string length for PEM data
        *cert_len = strlen(cert_out);
    }
    
    return err;
}

esp_err_t cloud_prov_get_private_key(char *key_out, size_t *key_len)
{
    if (key_out == NULL || key_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t required_size = CLOUD_PROV_MAX_KEY_SIZE;
    err = nvs_get_str(nvs_handle, NVS_KEY_PRIVATE, key_out, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        // nvs_get_str returns size including null terminator
        // Return the actual string length for PEM data
        *key_len = strlen(key_out);
    }
    
    return err;
}

esp_err_t cloud_prov_clear_certificates(void)
{
    ESP_LOGW(TAG, "Clearing stored certificates");
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    nvs_erase_key(nvs_handle, NVS_KEY_CERT);
    nvs_erase_key(nvs_handle, NVS_KEY_PRIVATE);
    nvs_erase_key(nvs_handle, NVS_KEY_CERT_ID);
    nvs_erase_key(nvs_handle, NVS_KEY_MQTT_CA);
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    return err;
}

esp_err_t cloud_prov_download_mqtt_ca_cert(void)
{
    // Check if certificate already exists
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        err = nvs_get_str(nvs_handle, NVS_KEY_MQTT_CA, NULL, &required_size);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK && required_size > 0) {
            ESP_LOGI(TAG, "MQTT CA certificate already exists (%zu bytes), skipping download", required_size);
            return ESP_OK;
        }
    }
    
    ESP_LOGI(TAG, "Downloading MQTT CA certificate from sensors.kannacloud.com...");
    
    esp_http_client_config_t config = {
        .url = "https://sensors.kannacloud.com/static/ca.crt",
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // Reset response buffer
    s_response_len = 0;
    memset(s_response_buffer, 0, sizeof(s_response_buffer));
    
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Server returned status: %d", status_code);
        return ESP_FAIL;
    }
    
    if (s_response_len == 0 || s_response_len >= CLOUD_PROV_MAX_CERT_SIZE) {
        ESP_LOGE(TAG, "Invalid certificate size: %zu bytes", s_response_len);
        return ESP_FAIL;
    }
    
    // Null-terminate the certificate
    s_response_buffer[s_response_len] = '\0';
    
    ESP_LOGI(TAG, "Downloaded MQTT CA certificate (%zu bytes)", s_response_len);
    
    // Store in NVS
    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS partition: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_MQTT_CA, s_response_buffer);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT CA certificate stored successfully");
    } else {
        ESP_LOGE(TAG, "Failed to store MQTT CA certificate: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t cloud_prov_get_mqtt_ca_cert(char *cert_out, size_t *cert_len)
{
    if (cert_out == NULL || cert_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t required_size = CLOUD_PROV_MAX_CERT_SIZE;
    err = nvs_get_str(nvs_handle, NVS_KEY_MQTT_CA, cert_out, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        *cert_len = strlen(cert_out);
    }
    
    return err;
}

/**
 * @brief Request certificate generation from SSL Manager
 */
static esp_err_t request_certificate_generation(char *cert_id_out, size_t cert_id_len)
{
    ESP_LOGI(TAG, "Requesting certificate generation from %s", CLOUD_PROV_SSL_MANAGER_URL);
    
    char device_id[32];
    cloud_prov_get_device_id(device_id, sizeof(device_id));
    
    // Build POST data (note: %% escapes the % in URL encoding %20 for spaces)
    char post_data[512];
    snprintf(post_data, sizeof(post_data),
             "cn=kc.local&organization=KannaCloud&org_unit=IoT%%20Sensors&"
             "locality=Casa%%20Grande&state=Arizona&country=US&"
             "email=devices@kannacloud.com&san=kc.local,DNS:*.local,IP:192.168.1.0/24");
    
    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = CLOUD_PROV_SSL_MANAGER_URL "/create",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = attach_cloud_api_key_header(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    // Reset response buffer
    s_response_len = 0;
    memset(s_response_buffer, 0, sizeof(s_response_buffer));
    
    // Perform request
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Server returned status: %d", status_code);
        ESP_LOGE(TAG, "Response: %.*s", (int)s_response_len, s_response_buffer);
        return ESP_FAIL;
    }
    
    // Parse JSON response
    s_response_buffer[s_response_len] = '\0';
    cJSON *json = cJSON_Parse(s_response_buffer);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }
    
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        ESP_LOGE(TAG, "Certificate ID not found in response");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    strncpy(cert_id_out, id_item->valuestring, cert_id_len - 1);
    cert_id_out[cert_id_len - 1] = '\0';
    
    ESP_LOGI(TAG, "Certificate generated with ID: %s", cert_id_out);
    
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief Download file from SSL Manager
 */
static esp_err_t download_file(const char *cert_id, const char *file_type, char *output, size_t output_size)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/download/%s/%s",
             CLOUD_PROV_SSL_MANAGER_URL, cert_id, file_type);
    
    ESP_LOGI(TAG, "Downloading %s from: %s", file_type, url);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    
    esp_err_t err = attach_cloud_api_key_header(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    
    // Reset response buffer
    s_response_len = 0;
    memset(s_response_buffer, 0, sizeof(s_response_buffer));
    
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    ESP_LOGI(TAG, "Download response - Status: %d, Length: %zu bytes", status_code, s_response_len);
    
    // Log first 200 chars of response for debugging
    if (s_response_len > 0) {
        char preview[201];
        size_t preview_len = s_response_len < 200 ? s_response_len : 200;
        memcpy(preview, s_response_buffer, preview_len);
        preview[preview_len] = '\0';
        ESP_LOGI(TAG, "Response preview: %.200s", preview);
    }
    
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "Failed to download %s: %s (status: %d)",
                 file_type, esp_err_to_name(err), status_code);
        return ESP_FAIL;
    }
    
    if (s_response_len >= output_size) {
        ESP_LOGE(TAG, "Downloaded %s too large (%zu bytes)", file_type, s_response_len);
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(output, s_response_buffer, s_response_len);
    output[s_response_len] = '\0';
    
    ESP_LOGI(TAG, "Downloaded %s (%zu bytes)", file_type, s_response_len);
    
    return ESP_OK;
}

esp_err_t cloud_prov_provision_device(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Starting automatic device provisioning");
    ESP_LOGI(TAG, "===========================================");
    
    // Check if already provisioned
    if (cloud_prov_has_certificates()) {
        ESP_LOGI(TAG, "Device already has certificates");
        if (s_callback) {
            s_callback(true, "Already provisioned");
        }
        return ESP_OK;
    }
    
    // Step 1: Request certificate generation
    char cert_id[64] = {0};
    esp_err_t err = request_certificate_generation(cert_id, sizeof(cert_id));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate certificate");
        if (s_callback) {
            s_callback(false, "Certificate generation failed");
        }
        return err;
    }
    
    // Step 2: Download private key
    char *private_key = malloc(CLOUD_PROV_MAX_KEY_SIZE);
    if (private_key == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for private key");
        return ESP_ERR_NO_MEM;
    }
    
    err = download_file(cert_id, "key", private_key, CLOUD_PROV_MAX_KEY_SIZE);
    if (err != ESP_OK) {
        free(private_key);
        if (s_callback) {
            s_callback(false, "Private key download failed");
        }
        return err;
    }
    
    // Step 3: Download certificate
    char *certificate = malloc(CLOUD_PROV_MAX_CERT_SIZE);
    if (certificate == NULL) {
        free(private_key);
        ESP_LOGE(TAG, "Failed to allocate memory for certificate");
        return ESP_ERR_NO_MEM;
    }
    
    err = download_file(cert_id, "cert", certificate, CLOUD_PROV_MAX_CERT_SIZE);
    if (err != ESP_OK) {
        free(private_key);
        free(certificate);
        if (s_callback) {
            s_callback(false, "Certificate download failed");
        }
        return err;
    }
    
    // Step 3.5: Download CA certificate
    char *ca_certificate = malloc(CLOUD_PROV_MAX_CERT_SIZE);
    if (ca_certificate == NULL) {
        free(private_key);
        free(certificate);
        ESP_LOGE(TAG, "Failed to allocate memory for CA certificate");
        return ESP_ERR_NO_MEM;
    }
    
    err = download_file(cert_id, "ca", ca_certificate, CLOUD_PROV_MAX_CERT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CA certificate download failed (optional): %s", esp_err_to_name(err));
        // CA cert is optional, don't fail provisioning
        free(ca_certificate);
        ca_certificate = NULL;
    } else {
        ESP_LOGI(TAG, "CA certificate downloaded successfully");
    }
    
    // Step 4: Initialize NVS partition and store certificates
    ESP_LOGI(TAG, "Initializing certificate NVS partition");
    err = nvs_flash_init_partition(NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing certificate NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_PARTITION));
        err = nvs_flash_init_partition(NVS_PARTITION);
    }
    if (err != ESP_OK) {
        free(private_key);
        free(certificate);
        ESP_LOGE(TAG, "Failed to initialize NVS partition: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Storing certificates in NVS");
    nvs_handle_t nvs_handle;
    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        free(private_key);
        free(certificate);
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // Store certificate ID
    nvs_set_str(nvs_handle, NVS_KEY_CERT_ID, cert_id);
    
    // Store private key
    err = nvs_set_str(nvs_handle, NVS_KEY_PRIVATE, private_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store private key: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        free(private_key);
        free(certificate);
        return err;
    }
    
    // Store certificate
    err = nvs_set_str(nvs_handle, NVS_KEY_CERT, certificate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store certificate: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        free(private_key);
        free(certificate);
        if (ca_certificate) free(ca_certificate);
        return err;
    }
    
    // Store CA certificate if available
    if (ca_certificate != NULL) {
        err = nvs_set_str(nvs_handle, "ca_cert", ca_certificate);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to store CA certificate: %s", esp_err_to_name(err));
            // Don't fail provisioning if CA cert storage fails
        } else {
            ESP_LOGI(TAG, "CA certificate stored in NVS");
        }
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    // Clean up
    free(private_key);
    free(certificate);
    if (ca_certificate) free(ca_certificate);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "✓ Device provisioning completed successfully!");
        ESP_LOGI(TAG, "===========================================");
        
        if (s_callback) {
            s_callback(true, "Provisioning completed");
        }
    } else {
        ESP_LOGE(TAG, "Failed to commit certificates to NVS");
        if (s_callback) {
            s_callback(false, "NVS storage failed");
        }
    }
    
    return err;
}
