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
                             "idf_provisioning.c"
                             "wifi_manager.c"
                             "provisioning_state.c"
                             "security.c"
                             "reset_button.c"
                             "time_sync.c"
                             "cloud_provisioning.c"
                             "http_server.c"
                             "http/http_websocket.c"
                             "api_key_manager.c"
                             "mdns_service.c"
                             "mqtt_telemetry.c"
                             "i2c_scanner.c"
                             "max17048.c"
                             "ezo_sensor.c"
                             "sensor_manager.c"
                             "web_file_editor.c"
                       INCLUDE_DIRS "." "http"
                       EMBED_TXTFILES ${WEB_FILES}
                       REQUIRES nvs_flash bt esp_wifi wifi_provisioning json esp_partition bootloader_support efuse driver esp_http_client app_update mqtt spi_flash esp_psram ${WEB_COMPONENTS})
