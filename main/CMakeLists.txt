# Conditionally embed web files only for ESP32-S3 (local dashboard)
if(NOT "${IDF_TARGET}" STREQUAL "esp32c6")
    set(WEB_FILES "web/index.html" "web/dashboard.css" "web/dashboard.js")
    set(WEB_COMPONENTS esp_http_server esp_https_server mdns fatfs wear_levelling)
else()
    set(WEB_FILES "")
    set(WEB_COMPONENTS "")
endif()

set(RUNTIME_CONFIG_HASH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/generate_runtime_config_hash.py")
set(RUNTIME_CONFIG_HASH_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RUNTIME_CONFIG_HASH_HEADER "${RUNTIME_CONFIG_HASH_DIR}/runtime_config_hash.h")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(MAKE_DIRECTORY "${RUNTIME_CONFIG_HASH_DIR}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_custom_command(
        OUTPUT "${RUNTIME_CONFIG_HASH_HEADER}"
        COMMAND ${Python3_EXECUTABLE} "${RUNTIME_CONFIG_HASH_SCRIPT}"
                --services "${CMAKE_SOURCE_DIR}/config/runtime/services.json"
                --keys "${CMAKE_SOURCE_DIR}/config/runtime/api_keys.json"
                --output "${RUNTIME_CONFIG_HASH_HEADER}"
        DEPENDS "${RUNTIME_CONFIG_HASH_SCRIPT}" "${CMAKE_SOURCE_DIR}/config/runtime/services.json" "${CMAKE_SOURCE_DIR}/config/runtime/api_keys.json"
        COMMENT "Generating runtime config hash header"
    )

    add_custom_target(runtime_config_hash DEPENDS "${RUNTIME_CONFIG_HASH_HEADER}")
endif()

idf_component_register(SRCS "main.c"
                             "common/error_handler.c"
                             "common/secure_logging.c"
                             "platform/chip_info.c"
                             "provisioning/idf_provisioning.c"
                             "provisioning/wifi_manager.c"
                             "provisioning/provisioning_state.c"
                             "provisioning/provisioning_wifi_ops_default.c"
                             "platform/security.c"
                             "platform/reset_button.c"
                             "boot/boot_monitor.c"
                             "boot/boot_coordinator.c"
                             "boot/boot_handlers.c"
                             "config/runtime_config.c"
                             "provisioning/provisioning_runner.c"
                             "services/provisioning/cloud_provisioning.c"
                             "services/http/http_server.c"
                             "services/http/http_handlers_sensors.c"
                             "services/http/http_websocket.c"
                             "services/keys/api_key_manager.c"
                             "services/discovery/mdns_service.c"
                             "services/telemetry/mqtt_payload_builder.c"
                             "services/telemetry/mqtt_publish_loop.c"
                             "services/telemetry/mqtt_connection_controller.c"
                             "services/telemetry/mqtt_publish_scheduler.c"
                             "services/telemetry/mqtt_connection_watchdog.c"
                             "services/telemetry/telemetry_pipeline.c"
                             "services/telemetry/mqtt_telemetry.c"
                             "platform/i2c_scanner.c"
                             "sensors/drivers/max17048.c"
                             "sensors/drivers/ezo_sensor.c"
                             "sensors/sensor_manager.c"
                             "sensors/config.c"
                             "sensors/pipeline.c"
                             "services/core/services.c"
                             "services/core/services_config.c"
                             "services/core/services_provisioning_bridge.c"
                             "network/network_boot.c"
                             "network/connectivity_watchdog.c"
                             "sensors/sensor_boot.c"
                                                         "services/http/web_file_editor.c"
                                                         "services/logging/log_history.c"
                             "services/time_sync/time_sync.c"
                                                         "storage/sd_logger.c"
                                                             INCLUDE_DIRS "." "services/http" "services/logging" "${RUNTIME_CONFIG_HASH_DIR}"
                                                             EMBED_TXTFILES ${WEB_FILES} "${CMAKE_SOURCE_DIR}/config/runtime/services.json" "${CMAKE_SOURCE_DIR}/config/runtime/api_keys.json"
                                                             REQUIRES nvs_flash bt esp_wifi wifi_provisioning json esp_partition bootloader_support efuse driver esp_http_client app_update mqtt spi_flash esp_psram fatfs sdmmc ${WEB_COMPONENTS})
if(TARGET runtime_config_hash)
        add_dependencies(${COMPONENT_LIB} runtime_config_hash)
endif()
add_dependencies(${COMPONENT_LIB} runtime_config_hash)
