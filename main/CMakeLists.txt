# Conditionally embed web files only for ESP32-S3 (local dashboard)
if(NOT "${IDF_TARGET}" STREQUAL "esp32c6")
    set(WEB_FILES "web/index.html" "web/dashboard.css" "web/dashboard.js")
    set(WEB_COMPONENTS esp_http_server esp_https_server mdns fatfs wear_levelling)
else()
    set(WEB_FILES "")
    set(WEB_COMPONENTS "")
endif()

idf_component_register(SRCS "main.c" 
                             "chip_info.c"
                             "provisioning/idf_provisioning.c"
                             "wifi_manager.c"
                             "provisioning/provisioning_state.c"
                             "security.c"
                             "reset_button.c"
                             "boot/boot_monitor.c"
                             "boot/boot_coordinator.c"
                             "boot/boot_handlers.c"
                             "provisioning/provisioning_runner.c"
                             "cloud_provisioning.c"
                             "services/http/http_server.c"
                             "services/http/http_handlers_sensors.c"
                             "services/http/http_websocket.c"
                             "api_key_manager.c"
                             "mdns_service.c"
                             "services/telemetry/mqtt_telemetry.c"
                             "i2c_scanner.c"
                             "sensors/drivers/max17048.c"
                             "sensors/drivers/ezo_sensor.c"
                             "sensors/sensor_manager.c"
                             "sensors/config.c"
                             "sensors/pipeline.c"
                             "services/core/services.c"
                             "network/network_boot.c"
                             "sensors/sensor_boot.c"
                             "services/http/web_file_editor.c"
                             "services/time_sync/time_sync.c"
                       INCLUDE_DIRS "." "services/http"
                       EMBED_TXTFILES ${WEB_FILES}
                       REQUIRES nvs_flash bt esp_wifi wifi_provisioning json esp_partition bootloader_support efuse driver esp_http_client app_update mqtt spi_flash esp_psram ${WEB_COMPONENTS})
