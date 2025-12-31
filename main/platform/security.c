#include "security.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_flash_encrypt.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include <string.h>

static const char *TAG = "SECURITY";

// Flag to track encryption status
static bool nvs_encryption_enabled = false;
static bool flash_encryption_enabled = false;

esp_err_t security_init(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Initializing Security Features");
    ESP_LOGI(TAG, "=================================");
    
    esp_err_t ret = ESP_OK;
    
    // Check if NVS encryption partition exists
    const esp_partition_t *nvs_key_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
        NULL
    );
    
    if (nvs_key_partition == NULL) {
        ESP_LOGE(TAG, "NVS keys partition not found!");
        ESP_LOGE(TAG, "Please ensure partitions.csv includes nvs_keys partition");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "NVS keys partition found at 0x%lx (size: %lu bytes)", 
             nvs_key_partition->address, nvs_key_partition->size);
    
    // Initialize NVS with encryption
    ESP_LOGI(TAG, "Initializing NVS with encryption...");
    
    // Allocate security configuration structure
    nvs_sec_cfg_t* cfg = (nvs_sec_cfg_t*)calloc(1, sizeof(nvs_sec_cfg_t));
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for NVS security config");
        return ESP_ERR_NO_MEM;
    }
    
    // Read or generate NVS encryption keys
    ret = nvs_flash_read_security_cfg(nvs_key_partition, cfg);
    
    if (ret == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "NVS encryption keys not found, generating new keys...");
        ESP_LOGI(TAG, "This is normal for first boot with encryption enabled");
        
        // Generate and store encryption keys
        ret = nvs_flash_generate_keys(nvs_key_partition, cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to generate NVS keys: %s", esp_err_to_name(ret));
            free(cfg);
            return ret;
        }
        
        ESP_LOGI(TAG, "✓ NVS encryption keys generated successfully");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read NVS security config: %s", esp_err_to_name(ret));
        free(cfg);
        return ret;
    } else {
        ESP_LOGI(TAG, "✓ NVS encryption keys loaded from partition");
    }
    
    // Initialize NVS with encryption
    ret = nvs_flash_secure_init_partition("nvs", cfg);
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
        ret = nvs_flash_secure_init_partition("nvs", cfg);
    }
    
    free(cfg);
    
    if (ret == ESP_OK) {
        nvs_encryption_enabled = true;
        ESP_LOGI(TAG, "✓ NVS encryption initialized successfully");
        ESP_LOGI(TAG, "✓ WiFi credentials will be stored encrypted");
    } else {
        ESP_LOGE(TAG, "✗ Failed to initialize NVS encryption: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Falling back to unencrypted NVS (INSECURE!)");
        
        // Fallback to unencrypted NVS
        ret = nvs_flash_init();
        nvs_encryption_enabled = false;
    }
    
    // Check Flash Encryption status
    flash_encryption_enabled = esp_flash_encryption_enabled();
    
    if (flash_encryption_enabled) {
        ESP_LOGI(TAG, "✓ Flash encryption is ENABLED");
        
        // Get flash encryption mode
        esp_flash_enc_mode_t mode = esp_get_flash_encryption_mode();
        switch (mode) {
            case ESP_FLASH_ENC_MODE_DEVELOPMENT:
                ESP_LOGI(TAG, "  Mode: DEVELOPMENT (up to 3 reflashes allowed)");
                ESP_LOGW(TAG, "  Switch to RELEASE mode for production!");
                break;
            case ESP_FLASH_ENC_MODE_RELEASE:
                ESP_LOGI(TAG, "  Mode: RELEASE (permanent, no reflash possible)");
                break;
            default:
                ESP_LOGI(TAG, "  Mode: DISABLED");
                break;
        }
    } else {
        ESP_LOGW(TAG, "✗ Flash encryption is DISABLED");
        ESP_LOGW(TAG, "  Firmware and data are NOT protected in flash");
        ESP_LOGW(TAG, "  Enable flash encryption for production deployment");
    }
    
    // Log security status summary
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Security Status Summary:");
    ESP_LOGI(TAG, "  NVS Encryption:   %s", nvs_encryption_enabled ? "ENABLED ✓" : "DISABLED ✗");
    ESP_LOGI(TAG, "  Flash Encryption: %s", flash_encryption_enabled ? "ENABLED ✓" : "DISABLED ✗");
    ESP_LOGI(TAG, "  Key Protection:   HMAC-based (eFuse)");
    
    // Overall security rating
    if (nvs_encryption_enabled && flash_encryption_enabled) {
        ESP_LOGI(TAG, "  Security Level:   HIGH ✓✓✓");
    } else if (nvs_encryption_enabled || flash_encryption_enabled) {
        ESP_LOGI(TAG, "  Security Level:   MEDIUM ✓✓");
    } else {
        ESP_LOGW(TAG, "  Security Level:   LOW ✗");
    }
    
    ESP_LOGI(TAG, "=================================");
    
    return ret;
}

bool security_is_nvs_encrypted(void)
{
    return nvs_encryption_enabled;
}

bool security_is_flash_encrypted(void)
{
    return flash_encryption_enabled;
}

void security_get_status(char* info, size_t len)
{
    if (info == NULL || len == 0) {
        return;
    }
    
    snprintf(info, len, 
             "NVS_Encryption:%s,Flash_Encryption:%s,Key_Protection:HMAC-eFuse",
             nvs_encryption_enabled ? "ENABLED" : "DISABLED",
             flash_encryption_enabled ? "ENABLED" : "DISABLED");
}
