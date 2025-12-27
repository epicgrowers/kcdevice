/**
 * @file web_file_editor.c
 * @brief Web file editor API for FATFS-based dashboard customization
 */

#include "web_file_editor.h"
#include "esp_log.h"

static const char *TAG = "WEB_EDITOR";

#ifndef CONFIG_IDF_TARGET_ESP32C6
// Full web file editor implementation for ESP32-S3

#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// External references to embedded web assets
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t dashboard_css_start[] asm("_binary_dashboard_css_start");
extern const uint8_t dashboard_css_end[]   asm("_binary_dashboard_css_end");
extern const uint8_t dashboard_js_start[]  asm("_binary_dashboard_js_start");
extern const uint8_t dashboard_js_end[]    asm("_binary_dashboard_js_end");

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} default_asset_t;

static const default_asset_t k_default_assets[] = {
    { "index.html",    index_html_start,    index_html_end    },
    { "dashboard.css", dashboard_css_start, dashboard_css_end },
    { "dashboard.js",  dashboard_js_start,  dashboard_js_end  }
};

static esp_err_t write_default_asset_internal(const default_asset_t *asset, bool allow_format);
static esp_err_t web_editor_mount_fs(bool format_if_needed);
static void web_editor_unmount_fs(void);
static esp_err_t web_editor_format_partition(void);
static esp_err_t web_editor_seed_all_defaults(void);

static esp_err_t write_default_asset(const default_asset_t *asset)
{
    return write_default_asset_internal(asset, true);
}

static const default_asset_t *find_default_asset(const char *filename)
{
    if (filename == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < (sizeof(k_default_assets) / sizeof(k_default_assets[0])); i++) {
        if (strcmp(filename, k_default_assets[i].name) == 0) {
            return &k_default_assets[i];
        }
    }
    return NULL;
}

static esp_err_t write_default_asset_internal(const default_asset_t *asset, bool allow_format)
{
    if (asset == NULL || asset->start == NULL || asset->end == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t fs_ready = web_editor_mount_fs(false);
    if (fs_ready != ESP_OK) {
        ESP_LOGE(TAG, "Cannot seed default asset %s: FATFS unavailable (%s)",
                 asset->name, esp_err_to_name(fs_ready));
        return fs_ready;
    }

    size_t asset_size = asset->end - asset->start;
    if (asset_size == 0) {
        ESP_LOGW(TAG, "Default asset %s has zero length, skipping", asset->name);
        return ESP_ERR_INVALID_SIZE;
    }

    if (asset->start[asset_size - 1] == '\0') {
        ESP_LOGD(TAG, "Trimming trailing null terminator for %s", asset->name);
        asset_size--;
    }

    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", WEB_EDITOR_FS_PATH, asset->name);

    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing (errno=%d)", filepath, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(asset->start, 1, asset_size, f);
    fclose(f);

    if (written != asset_size) {
        int errsv = errno;
        ESP_LOGE(TAG, "Failed to seed %s (written %zu/%zu bytes, errno=%d: %s)",
                 asset->name, written, asset_size, errsv, strerror(errsv));

        if (allow_format) {
            uint64_t total_bytes = 0;
            uint64_t free_bytes = 0;
            if (esp_vfs_fat_info(WEB_EDITOR_FS_PATH, &total_bytes, &free_bytes) == ESP_OK) {
                ESP_LOGW(TAG, "FATFS usage before format: used=%" PRIu64 " KB free=%" PRIu64 " KB",
                         (total_bytes - free_bytes) / 1024, free_bytes / 1024);
            }

            ESP_LOGW(TAG, "Formatting FATFS partition 'www' to recover web assets");
            web_editor_unmount_fs();
            esp_err_t fmt = web_editor_format_partition();
            if (fmt != ESP_OK) {
                ESP_LOGE(TAG, "FATFS format failed: %s", esp_err_to_name(fmt));
                return ESP_FAIL;
            }

            esp_err_t remount = web_editor_mount_fs(true);
            if (remount != ESP_OK) {
                ESP_LOGE(TAG, "Failed to remount FATFS after format: %s", esp_err_to_name(remount));
                return remount;
            }

            ESP_LOGI(TAG, "FATFS format complete, reseeding default dashboard files");
            esp_err_t requested_result = ESP_FAIL;
            for (size_t i = 0; i < (sizeof(k_default_assets) / sizeof(k_default_assets[0])); i++) {
                const default_asset_t *current = &k_default_assets[i];
                esp_err_t res = write_default_asset_internal(current, false);
                if (current == asset) {
                    requested_result = res;
                }
            }
            return requested_result;
        }

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Seeded default %s (%zu bytes)", asset->name, asset_size);
    return ESP_OK;
}

static void ensure_default_asset(const default_asset_t *asset)
{
    if (asset == NULL) {
        return;
    }

    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", WEB_EDITOR_FS_PATH, asset->name);

    struct stat st;
    bool needs_restore = false;
    if (stat(filepath, &st) != 0) {
        ESP_LOGI(TAG, "%s missing, seeding default copy", asset->name);
        needs_restore = true;
    } else if (st.st_size == 0) {
        ESP_LOGW(TAG, "%s exists but is empty, restoring default copy", asset->name);
        needs_restore = true;
    }

    if (!needs_restore) {
        return;
    }

    write_default_asset(asset);
}

static esp_err_t web_editor_seed_all_defaults(void)
{
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < (sizeof(k_default_assets) / sizeof(k_default_assets[0])); i++) {
        esp_err_t res = write_default_asset(&k_default_assets[i]);
        if (res != ESP_OK && first_error == ESP_OK) {
            first_error = res;
        }
    }
    return first_error;
}

static esp_err_t web_editor_mount_fs(bool format_if_needed)
{
    if (s_wl_handle != WL_INVALID_HANDLE) {
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 5,
        .allocation_unit_size = 4096
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(WEB_EDITOR_FS_PATH, "www", &mount_config, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (err=%s)", esp_err_to_name(ret));
        return ret;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(WEB_EDITOR_FS_PATH, &total_bytes, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "FATFS: total=%" PRIu64 " KB, free=%" PRIu64 " KB",
                 total_bytes / 1024, free_bytes / 1024);
    }

    return ESP_OK;
}

static void web_editor_unmount_fs(void)
{
    if (s_wl_handle != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(WEB_EDITOR_FS_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }
}

static esp_err_t web_editor_format_partition(void)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                                "www");
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition 'www' not found; cannot format FATFS volume");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "Erasing FATFS partition 'www' (%lu bytes)", (unsigned long)partition->size);
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Partition erase failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t web_editor_init_fs(void)
{
    ESP_LOGI(TAG, "Initializing FATFS dashboard volume...");

    esp_err_t ret = web_editor_mount_fs(true);
    if (ret != ESP_OK) {
        return ret;
    }

    // Seed dashboard defaults (HTML/CSS/JS) if missing or empty
    for (size_t i = 0; i < (sizeof(k_default_assets) / sizeof(k_default_assets[0])); i++) {
        ensure_default_asset(&k_default_assets[i]);
    }
    
    return ESP_OK;
}

esp_err_t web_editor_load_file(const char *filename, char **content, size_t *size)
{
    if (filename == NULL || content == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t fs_status = web_editor_mount_fs(false);
    if (fs_status != ESP_OK) {
        ESP_LOGE(TAG, "FATFS volume unavailable when loading %s: %s", filename, esp_err_to_name(fs_status));
        return fs_status;
    }
    
    // Security: prevent directory traversal
    if (strstr(filename, "..") != NULL || strchr(filename, '/') != NULL) {
        ESP_LOGW(TAG, "Invalid filename: %s", filename);
        return ESP_ERR_INVALID_ARG;
    }
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", WEB_EDITOR_FS_PATH, filename);
    
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        int errsv = errno;
        ESP_LOGW(TAG, "Failed to open %s (errno=%d)", filepath, errsv);

        const default_asset_t *asset = find_default_asset(filename);
        if (asset != NULL) {
            ESP_LOGI(TAG, "Restoring embedded default for %s", filename);
            if (write_default_asset(asset) == ESP_OK) {
                return web_editor_load_file(filename, content, size);
            }
            ESP_LOGE(TAG, "Restore failed for %s (errno=%d)", filename, errsv);
        }

        return (errsv == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        const default_asset_t *asset = find_default_asset(filename);
        if (asset != NULL) {
            ESP_LOGW(TAG, "%s appears empty, restoring embedded default", filename);
            if (write_default_asset(asset) == ESP_OK) {
                // Try loading again after restore
                return web_editor_load_file(filename, content, size);
            }
        }
        ESP_LOGE(TAG, "File %s is empty and no default available", filename);
        return ESP_FAIL;
    }
    
    if (fsize > WEB_EDITOR_MAX_FILE_SIZE) {
        fclose(f);
        ESP_LOGW(TAG, "File too large: %ld bytes", fsize);
        return ESP_ERR_INVALID_SIZE;
    }
    
    *content = malloc(fsize + 1);
    if (*content == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_size = fread(*content, 1, fsize, f);
    fclose(f);

    if (read_size == 0 || read_size != (size_t)fsize) {
        ESP_LOGW(TAG, "Read %zu/%ld bytes from %s, treating as corruption", read_size, fsize, filename);
        free(*content);
        *content = NULL;
        *size = 0;

        const default_asset_t *asset = find_default_asset(filename);
        if (asset != NULL && write_default_asset(asset) == ESP_OK) {
            ESP_LOGI(TAG, "Retried load after restoring %s", filename);
            return web_editor_load_file(filename, content, size);
        }
        return ESP_FAIL;
    }

    (*content)[read_size] = 0;
    *size = read_size;
    
    ESP_LOGI(TAG, "Loaded file: %s (%zu bytes)", filename, *size);
    return ESP_OK;
}

esp_err_t web_editor_save_file(const char *filename, const char *content, size_t size)
{
    if (filename == NULL || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t fs_status = web_editor_mount_fs(false);
    if (fs_status != ESP_OK) {
        ESP_LOGE(TAG, "FATFS volume unavailable when saving %s: %s", filename, esp_err_to_name(fs_status));
        return fs_status;
    }
    
    // Security: prevent directory traversal
    if (strstr(filename, "..") != NULL || strchr(filename, '/') != NULL) {
        ESP_LOGW(TAG, "Invalid filename: %s", filename);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Only allow certain file types
    if (!strstr(filename, ".html") && !strstr(filename, ".js") && !strstr(filename, ".css")) {
        ESP_LOGW(TAG, "File type not allowed: %s", filename);
        return ESP_ERR_NOT_ALLOWED;
    }
    
    if (size > WEB_EDITOR_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "File too large: %zu bytes", size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", WEB_EDITOR_FS_PATH, filename);
    
    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(content, 1, size, f);
    fclose(f);
    
    if (written != size) {
        ESP_LOGE(TAG, "Write failed: %zu/%zu bytes written", written, size);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Saved file: %s (%zu bytes)", filename, size);
    return ESP_OK;
}

esp_err_t web_editor_list_files(char **json_output)
{
    if (json_output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t fs_status = web_editor_mount_fs(false);
    if (fs_status != ESP_OK) {
        ESP_LOGE(TAG, "FATFS volume unavailable when listing files: %s", esp_err_to_name(fs_status));
        return fs_status;
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    
    DIR *dir = opendir(WEB_EDITOR_FS_PATH);
    if (dir == NULL) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Regular file
            cJSON *file_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(file_obj, "name", entry->d_name);
            
            char filepath[300];  // Increased buffer size to accommodate path + filename
            snprintf(filepath, sizeof(filepath), "%s/%s", WEB_EDITOR_FS_PATH, entry->d_name);
            
            struct stat st;
            if (stat(filepath, &st) == 0) {
                cJSON_AddNumberToObject(file_obj, "size", st.st_size);
            }
            
            cJSON_AddItemToArray(files, file_obj);
        }
    }
    closedir(dir);
    
    cJSON_AddItemToObject(root, "files", files);
    
    *json_output = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return (*json_output != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

const char* web_editor_get_content_type(const char *filename)
{
    if (strstr(filename, ".html")) {
        return "text/html";
    } else if (strstr(filename, ".js")) {
        return "application/javascript";
    } else if (strstr(filename, ".css")) {
        return "text/css";
    }
    return "text/plain";
}

esp_err_t web_editor_reset_fs(void)
{
    ESP_LOGW(TAG, "Resetting FATFS volume '/www' and restoring default dashboard assets");

    web_editor_unmount_fs();

    esp_err_t err = web_editor_format_partition();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATFS reset failed during erase: %s", esp_err_to_name(err));
        return err;
    }

    err = web_editor_mount_fs(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATFS reset failed during remount: %s", esp_err_to_name(err));
        return err;
    }

    err = web_editor_seed_all_defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reseed default dashboard files: %s", esp_err_to_name(err));
    }
    return err;
}

#else
// ESP32-C6: Stub implementations (no web file editor)

esp_err_t web_editor_init_fs(void)
{
    ESP_LOGW(TAG, "Web file editor not available on ESP32-C6 (cloud-only mode)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t web_editor_load_file(const char *filename, char **content, size_t *size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t web_editor_save_file(const char *filename, const char *content, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t web_editor_list_files(char **json_output)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool web_editor_file_exists(const char *filename)
{
    return false;
}

esp_err_t web_editor_reset_fs(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_IDF_TARGET_ESP32C6
