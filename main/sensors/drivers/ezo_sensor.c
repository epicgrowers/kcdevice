/**
 * @file ezo_sensor.c
 * @brief Atlas Scientific EZO sensor driver implementation
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ezo_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSORS:EZO";

static esp_err_t ezo_sensor_receive_response(ezo_sensor_t *sensor, char *response, size_t response_size);
static esp_err_t ezo_sensor_parse_values(char *response, float values[4], uint8_t *count);

/**
 * @brief Send command and read response from EZO sensor
 */
esp_err_t ezo_sensor_send_command(ezo_sensor_t *sensor, const char *command, 
                                   char *response, size_t response_size, uint32_t delay_ms) {
    if (sensor == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Sending command to 0x%02X: %s", sensor->config.i2c_address, command);

    // Send command
    esp_err_t ret = i2c_master_transmit(sensor->dev_handle, (const uint8_t *)command, 
                                        strlen(command), EZO_RESPONSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check if this is an I2C address change command (device will reboot)
    if (strncmp(command, "I2C,", 4) == 0) {
        ESP_LOGW(TAG, "I2C address change command sent - device will reboot");
        return ESP_OK;
    }

    // Wait for sensor to process command
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    // Read response if buffer provided
    if (response != NULL && response_size > 0) {
        return ezo_sensor_receive_response(sensor, response, response_size);
    }

    return ESP_OK;
}

static esp_err_t ezo_sensor_receive_response(ezo_sensor_t *sensor, char *response, size_t response_size) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buffer[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = i2c_master_receive(sensor->dev_handle, buffer, EZO_LARGEST_STRING, EZO_RESPONSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read response: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t status = buffer[0];
    if (status == EZO_RESP_SUCCESS) {
        if (response != NULL && response_size > 0) {
            size_t copy_len = (response_size - 1 < EZO_LARGEST_STRING - 1) ?
                              response_size - 1 : EZO_LARGEST_STRING - 1;
            size_t j = 0;
            for (size_t i = 1; i < EZO_LARGEST_STRING && j < copy_len && buffer[i] != 0; i++) {
                response[j++] = (char)buffer[i];
            }
            response[j] = '\0';
        }
        return ESP_OK;
    }

    if (status == EZO_RESP_SYNTAX_ERROR) {
        ESP_LOGE(TAG, "Syntax error in command response");
        return ESP_ERR_INVALID_ARG;
    }
    if (status == EZO_RESP_NOT_READY) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (status == EZO_RESP_NO_DATA) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGE(TAG, "Unknown response code: 0x%02X", status);
    return ESP_FAIL;
}

static esp_err_t ezo_sensor_parse_values(char *response, float values[4], uint8_t *count) {
    if (response == NULL || values == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    char *token = strtok(response, ",");
    while (token != NULL && *count < 4) {
        if (token[0] == '-' || token[0] == '.' || (token[0] >= '0' && token[0] <= '9')) {
            values[*count] = atof(token);
            (*count)++;
        }
        token = strtok(NULL, ",");
    }

    return ESP_OK;
}

/**
 * @brief Initialize EZO sensor
 */
esp_err_t ezo_sensor_init(ezo_sensor_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t i2c_address) {
    if (sensor == NULL || bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing EZO sensor at address 0x%02X", i2c_address);

    // Initialize configuration
    memset(&sensor->config, 0, sizeof(ezo_sensor_config_t));
    sensor->config.i2c_address = i2c_address;
    sensor->bus_handle = bus_handle;
    sensor->config.temp_compensation = 25.0f;
    sensor->config.temp_comp_valid = false;
    sensor->config.calibration_status[0] = '\0';
    sensor->config.calibration_status_valid = false;

    // Create I2C device handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_address,
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &sensor->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    // CRITICAL: Wait for any pending response from previous boot to fully arrive
    // Some EZO sensors (especially HUM) may have slow-arriving stale responses
    vTaskDelay(pdMS_TO_TICKS(600));  // Wait 600ms for any pending data to arrive

    // Clear any pending data from previous commands or boot cycles
    // EZO sensors can have stale responses in buffer from previous sessions
    char dummy[EZO_LARGEST_STRING] = {0};
    int cleared_count = 0;
    for (int i = 0; i < 5; i++) {  // Try up to 5 times to ensure buffer is fully cleared
        esp_err_t flush_ret = ezo_sensor_receive_response(sensor, dummy, sizeof(dummy));
        if (flush_ret == ESP_ERR_NOT_FINISHED) {
            break;  // Buffer is empty
        }
        cleared_count++;
        ESP_LOGW(TAG, "Cleared stale response #%d from 0x%02X: '%s'", cleared_count, i2c_address, dummy);
    }
    if (cleared_count > 0) {
        ESP_LOGI(TAG, "Successfully cleared %d stale response(s) from 0x%02X", cleared_count, i2c_address);
    }

    // Get device information with retries for slow sensors
    const int max_retries = 3;
    for (int i = 0; i < max_retries; i++) {
        ret = ezo_sensor_get_device_info(sensor);
        if (ret == ESP_OK) {
            break;
        }
        if (ret == ESP_ERR_NOT_FINISHED && i < max_retries - 1) {
            ESP_LOGW(TAG, "Sensor not ready, retrying in 2 seconds... (attempt %d/%d)", i + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else if (i == max_retries - 1) {
            ESP_LOGW(TAG, "Failed to get device info after %d retries, continuing anyway", max_retries);
        }
    }

    // Validate type field after all initialization
    size_t type_len = strnlen(sensor->config.type, sizeof(sensor->config.type));
    if (type_len == 0 || type_len >= sizeof(sensor->config.type)) {
        ESP_LOGE(TAG, "Address 0x%02X: CRITICAL - type field corrupted after init! len=%d, content='%.*s'", 
                 sensor->config.i2c_address, type_len, sizeof(sensor->config.type), sensor->config.type);
        // Force to empty string to trigger UNKNOWN fallback later
        sensor->config.type[0] = '\0';
    }

    ESP_LOGI(TAG, "EZO sensor initialized: Type=%s, FW=%s", 
             sensor->config.type, sensor->config.firmware_version);

    return ESP_OK;
}

esp_err_t ezo_sensor_refresh_settings(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t overall = ESP_OK;

    if (sensor->config.capability_flags & EZO_CAP_CALIBRATION) {
        char status[sizeof(sensor->config.calibration_status)] = {0};
        esp_err_t ret = ezo_sensor_get_calibration_status(sensor, status, sizeof(status));
        if (ret == ESP_OK) {
            strncpy(sensor->config.calibration_status, status, sizeof(sensor->config.calibration_status) - 1);
            sensor->config.calibration_status[sizeof(sensor->config.calibration_status) - 1] = '\0';
            sensor->config.calibration_status_valid = true;
        } else {
            sensor->config.calibration_status[0] = '\0';
            sensor->config.calibration_status_valid = false;
            if (overall == ESP_OK) {
                overall = ret;
            }
        }
    }

    if ((sensor->config.capability_flags & EZO_CAP_TEMP_COMP) && strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        float temp_c = 0.0f;
        esp_err_t ret = ezo_ph_get_temperature_comp(sensor, &temp_c);
        if (ret == ESP_OK) {
            sensor->config.temp_compensation = temp_c;
            sensor->config.temp_comp_valid = true;
        } else {
            sensor->config.temp_comp_valid = false;
            if (overall == ESP_OK) {
                overall = ret;
            }
        }
    }

    if (sensor->config.capability_flags & EZO_CAP_MODE) {
        bool continuous = sensor->config.continuous_mode;
        esp_err_t ret = ezo_sensor_get_continuous_mode(sensor, &continuous);
        if (ret == ESP_OK) {
            sensor->config.continuous_mode = continuous;
        } else if (overall == ESP_OK) {
            overall = ret;
        }
    }

    // Re-query device info to refresh ALL settings including output parameters
    esp_err_t info_ret = ezo_sensor_get_device_info(sensor);
    if (info_ret != ESP_OK && overall == ESP_OK) {
        overall = info_ret;
    }

    // Sleep state cannot be queried; assume awake unless explicitly changed elsewhere.

    return overall;
}

/**
 * @brief Deinitialize EZO sensor
 */
esp_err_t ezo_sensor_deinit(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sensor->dev_handle != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(sensor->dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to remove I2C device: %s", esp_err_to_name(ret));
            return ret;
        }
        sensor->dev_handle = NULL;
    }

    return ESP_OK;
}

/**
 * @brief Get device information
 */
esp_err_t ezo_sensor_get_device_info(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    // Send info command
    esp_err_t ret = ezo_sensor_send_command(sensor, "i", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Address 0x%02X: Failed to send 'i' command: %s", 
                 sensor->config.i2c_address, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Address 0x%02X: Device info response: '%s'", sensor->config.i2c_address, response);

    // Parse response: ?I,<type>,<version>
    char *token = strtok(response, ",");
    int field = 0;
    
    while (token != NULL) {
        if (field == 0 && strcmp(token, "?I") == 0) {
            // Valid info response
        } else if (field == 1) {
            // Sensor type
            strncpy(sensor->config.type, token, EZO_MAX_SENSOR_TYPE - 1);
            sensor->config.type[EZO_MAX_SENSOR_TYPE - 1] = '\0';  // Ensure null termination
            ESP_LOGI(TAG, "Address 0x%02X: Parsed type = '%s'", sensor->config.i2c_address, sensor->config.type);
        } else if (field == 2) {
            // Firmware version
            strncpy(sensor->config.firmware_version, token, EZO_MAX_FW_VERSION - 1);
            sensor->config.firmware_version[EZO_MAX_FW_VERSION - 1] = '\0';  // Ensure null termination
        }
        token = strtok(NULL, ",");
        field++;
    }

    // Set capability defaults based on type
    sensor->config.capability_flags = 0;
    if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        sensor->config.capability_flags = EZO_CAP_CALIBRATION | EZO_CAP_TEMP_COMP |
                                          EZO_CAP_MODE | EZO_CAP_SLEEP;
    } else if (strcmp(sensor->config.type, EZO_TYPE_ORP) == 0) {
        sensor->config.capability_flags = EZO_CAP_CALIBRATION | EZO_CAP_MODE |
                                          EZO_CAP_SLEEP;
    } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
        sensor->config.capability_flags = EZO_CAP_CALIBRATION | EZO_CAP_MODE;
    } else if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
        sensor->config.capability_flags = EZO_CAP_CALIBRATION;
    } else if (strcmp(sensor->config.type, EZO_TYPE_DO) == 0) {
        sensor->config.capability_flags = EZO_CAP_CALIBRATION | EZO_CAP_MODE;
    }

    // Get sensor name
    ret = ezo_sensor_get_name(sensor, sensor->config.name, sizeof(sensor->config.name));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get sensor name");
    } else if (sensor->config.name[0] != '\0') {
        ESP_LOGI(TAG, "Address 0x%02X: Sensor name = '%s'", sensor->config.i2c_address, sensor->config.name);
    }

    // Get LED status
    ret = ezo_sensor_get_led(sensor, &sensor->config.led_control);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get LED status");
    }

    // Get protocol lock status
    ret = ezo_sensor_get_plock(sensor, &sensor->config.protocol_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get protocol lock status");
    }

    // Get sensor-specific parameters
    if (strcmp(sensor->config.type, EZO_TYPE_RTD) == 0) {
        ezo_rtd_get_scale(sensor, &sensor->config.rtd.temperature_scale);
    } else if (strcmp(sensor->config.type, EZO_TYPE_PH) == 0) {
        ezo_ph_get_extended_scale(sensor, &sensor->config.ph.extended_scale);
    } else if (strcmp(sensor->config.type, EZO_TYPE_EC) == 0) {
        ezo_ec_get_probe_type(sensor, &sensor->config.ec.probe_type);
        ezo_ec_get_tds_factor(sensor, &sensor->config.ec.tds_conversion_factor);
        
        // Query which output parameters are enabled
        char param_response[EZO_LARGEST_STRING] = {0};
        ret = ezo_sensor_send_command(sensor, "O,?", param_response, sizeof(param_response), EZO_SHORT_WAIT_MS);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Address 0x%02X: EC param response: '%s'", sensor->config.i2c_address, param_response);
            
            // Parse response: ?O,EC,TDS,S,SG or ?O,Cond,TDS,Sal,SG etc.
            sensor->config.ec.param_ec = false;
            sensor->config.ec.param_tds = false;
            sensor->config.ec.param_s = false;
            sensor->config.ec.param_sg = false;
            
            // Make a copy for strtok since it modifies the string
            char response_copy[EZO_LARGEST_STRING];
            strncpy(response_copy, param_response, sizeof(response_copy) - 1);
            response_copy[sizeof(response_copy) - 1] = '\0';
            
            char *param_token = strtok(response_copy, ",");
            int param_field = 0;
            
            while (param_token != NULL) {
                ESP_LOGI(TAG, "EC param field %d: '%s'", param_field, param_token);
                if (param_field == 0 && strcmp(param_token, "?O") == 0) {
                    // Valid output query response
                } else if (param_field > 0) {
                    // Check for various possible parameter names
                    if (strcmp(param_token, "EC") == 0 || strcmp(param_token, "Cond") == 0 || strcmp(param_token, "ec") == 0) {
                        sensor->config.ec.param_ec = true;
                        ESP_LOGI(TAG, "Detected EC parameter: '%s'", param_token);
                    } else if (strcmp(param_token, "TDS") == 0 || strcmp(param_token, "tds") == 0) {
                        sensor->config.ec.param_tds = true;
                        ESP_LOGI(TAG, "Detected TDS parameter: '%s'", param_token);
                    } else if (strcmp(param_token, "S") == 0 || strcmp(param_token, "Sal") == 0 || strcmp(param_token, "s") == 0) {
                        sensor->config.ec.param_s = true;
                        ESP_LOGI(TAG, "Detected Salinity parameter: '%s'", param_token);
                    } else if (strcmp(param_token, "SG") == 0 || strcmp(param_token, "sg") == 0) {
                        sensor->config.ec.param_sg = true;
                        ESP_LOGI(TAG, "Detected SG parameter: '%s'", param_token);
                    } else {
                        ESP_LOGW(TAG, "Unknown EC parameter: '%s'", param_token);
                    }
                }
                param_token = strtok(NULL, ",");
                param_field++;
            }
            
            ESP_LOGI(TAG, "EC outputs: EC=%d TDS=%d S=%d SG=%d", 
                     sensor->config.ec.param_ec, sensor->config.ec.param_tds,
                     sensor->config.ec.param_s, sensor->config.ec.param_sg);
        }
    } else if (strcmp(sensor->config.type, EZO_TYPE_HUM) == 0) {
        ESP_LOGI(TAG, "Address 0x%02X: Configuring HUM sensor (type verified: '%s')", 
                 sensor->config.i2c_address, sensor->config.type);
        
        // Query which output parameters are enabled
        char param_response[EZO_LARGEST_STRING] = {0};
        ret = ezo_sensor_send_command(sensor, "O,?", param_response, sizeof(param_response), EZO_SHORT_WAIT_MS);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Address 0x%02X: HUM param response: '%s'", sensor->config.i2c_address, param_response);
            
            // Parse response: ?O,HUM,T,Dew or ?O,HUM,Dew etc.
            // Reset counts
            sensor->config.hum.param_count = 0;
            sensor->config.hum.param_hum = false;
            sensor->config.hum.param_t = false;
            sensor->config.hum.param_dew = false;
            
            char *param_token = strtok(param_response, ",");
            int param_field = 0;
            
            while (param_token != NULL && sensor->config.hum.param_count < 4) {
                if (param_field == 0 && strcmp(param_token, "?O") == 0) {
                    // Valid output query response
                } else if (param_field > 0) {
                    // Store parameter name and set flag
                    strncpy(sensor->config.hum.param_order[sensor->config.hum.param_count], 
                           param_token, sizeof(sensor->config.hum.param_order[0]) - 1);
                    
                    if (strcmp(param_token, "HUM") == 0) {
                        sensor->config.hum.param_hum = true;
                    } else if (strcmp(param_token, "T") == 0) {
                        sensor->config.hum.param_t = true;
                    } else if (strcmp(param_token, "DEW") == 0) {
                        sensor->config.hum.param_dew = true;
                    }
                    
                    sensor->config.hum.param_count++;
                }
                param_token = strtok(NULL, ",");
                param_field++;
            }
            
            ESP_LOGI(TAG, "HUM sensor has %d parameters enabled", sensor->config.hum.param_count);
            ESP_LOGI(TAG, "Address 0x%02X: After HUM config, type still = '%s'", 
                     sensor->config.i2c_address, sensor->config.type);
        } else {
            ESP_LOGE(TAG, "Address 0x%02X: Failed to query HUM output parameters: %s", 
                     sensor->config.i2c_address, esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

/**
 * @brief Read sensor value
 */
esp_err_t ezo_sensor_read(ezo_sensor_t *sensor, float *value) {
    if (sensor == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "R", response, sizeof(response), EZO_LONG_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse the numeric response
    *value = atof(response);
    
    ESP_LOGI(TAG, "Sensor 0x%02X read: %.2f", sensor->config.i2c_address, *value);
    
    return ESP_OK;
}

/**
 * @brief Read all sensor values (for multi-value sensors like HUM)
 */
esp_err_t ezo_sensor_read_all(ezo_sensor_t *sensor, float values[4], uint8_t *count) {
    if (sensor == NULL || values == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "R", response, sizeof(response), EZO_LONG_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ezo_sensor_parse_values(response, values, count);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sensor 0x%02X read %d values: %.2f%s",
                 sensor->config.i2c_address,
                 *count,
                 *count > 0 ? values[0] : 0.0f,
                 *count > 1 ? ",..." : "");
    }
    return ret;
}

esp_err_t ezo_sensor_start_read(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return ezo_sensor_send_command(sensor, "R", NULL, 0, 0);
}

esp_err_t ezo_sensor_start_read_with_temp(ezo_sensor_t *sensor, float temp_celsius) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate temperature range (EZO sensors typically support -126 to 1254°C)
    if (temp_celsius < -126.0f || temp_celsius > 1254.0f) {
        ESP_LOGW(TAG, "Temperature %.2f°C out of range, using regular read", temp_celsius);
        return ezo_sensor_start_read(sensor);
    }
    
    // Format RT command with temperature
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "RT,%.2f", temp_celsius);
    
    return ezo_sensor_send_command(sensor, cmd, NULL, 0, 0);
}

esp_err_t ezo_sensor_fetch_all(ezo_sensor_t *sensor, float values[4], uint8_t *count) {
    if (sensor == NULL || values == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_receive_response(sensor, response, sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t parse_ret = ezo_sensor_parse_values(response, values, count);
    if (parse_ret == ESP_OK) {
        ESP_LOGI(TAG, "Sensor 0x%02X fetch %d values: %.2f%s",
                 sensor->config.i2c_address,
                 *count,
                 *count > 0 ? values[0] : 0.0f,
                 *count > 1 ? ",..." : "");
    }
    return parse_ret;
}

/**
 * @brief Get sensor name
 */
esp_err_t ezo_sensor_get_name(ezo_sensor_t *sensor, char *name, size_t name_size) {
    if (sensor == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "Name,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGD(TAG, "Get name response from 0x%02X: '%s'", sensor->config.i2c_address, response);

    // Initialize name to empty
    name[0] = '\0';

    // Parse response: ?NAME,<name> (note: uppercase NAME in response)
    // Response format: "?NAME,<name>" if name is set, or just status code if not set
    if (strstr(response, "?NAME,") == response || strstr(response, "?Name,") == response) {
        // Name is set, extract it (skip "?NAME," or "?Name,")
        const char *name_start = strchr(response, ',');
        if (name_start != NULL) {
            name_start++; // Skip the comma
            strncpy(name, name_start, name_size - 1);
            name[name_size - 1] = '\0';
            ESP_LOGD(TAG, "Parsed name from 0x%02X: '%s'", sensor->config.i2c_address, name);
        }
    } else if (strcmp(response, "?NAME") == 0 || strcmp(response, "?Name") == 0) {
        // No name set (some firmware versions may return just "?NAME" or "?Name")
        ESP_LOGD(TAG, "No name set for sensor 0x%02X", sensor->config.i2c_address);
    } else {
        // Status code only (no name set)
        ESP_LOGD(TAG, "No name set for sensor 0x%02X (response: '%s')", sensor->config.i2c_address, response);
    }

    return ESP_OK;
}

/**
 * @brief Set sensor name
 * 
 * Name must be 1-16 characters, alphanumeric and underscore only.
 * EZO sensors do not accept spaces, commas, or special characters.
 */
esp_err_t ezo_sensor_set_name(ezo_sensor_t *sensor, const char *name) {
    if (sensor == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check firmware version - Name command may not be supported in older firmware
    ESP_LOGI(TAG, "Sensor 0x%02X firmware: %s", sensor->config.i2c_address, sensor->config.firmware_version);

    // Validate name length
    size_t name_len = strnlen(name, EZO_MAX_SENSOR_NAME + 1);
    if (name_len == 0 || name_len > EZO_MAX_SENSOR_NAME) {
        ESP_LOGE(TAG, "Invalid name length: %d (must be 1-16)", name_len);
        return ESP_ERR_INVALID_ARG;
    }

    // Validate characters (alphanumeric and underscore only)
    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
              (c >= '0' && c <= '9') || c == '_')) {
            ESP_LOGE(TAG, "Invalid character '%c' in sensor name (only A-Z, a-z, 0-9, _ allowed)", c);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Pause sensor reading to avoid I2C conflicts (if not already paused)
    extern esp_err_t sensor_manager_pause_reading(void);
    extern bool sensor_manager_is_reading_in_progress(void);
    extern esp_err_t sensor_manager_resume_reading(void);
    extern bool sensor_manager_is_reading_paused(void);
    
    bool was_already_paused = sensor_manager_is_reading_paused();
    
    if (!was_already_paused) {
        sensor_manager_pause_reading();
    }
    
    // Wait for any in-progress reading to complete (max 3 seconds)
    int wait_count = 0;
    while (sensor_manager_is_reading_in_progress() && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
    if (sensor_manager_is_reading_in_progress()) {
        ESP_LOGW(TAG, "Timed out waiting for sensor reading to complete");
    }
    
    // Clear any stale data in sensor buffer before setting name
    char dummy[EZO_LARGEST_STRING];
    for (int i = 0; i < 3; i++) {
        ezo_sensor_receive_response(sensor, dummy, sizeof(dummy));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    char command[32];
    char response[EZO_LARGEST_STRING] = {0};
    snprintf(command, sizeof(command), "Name,%s", name);
    
    ESP_LOGI(TAG, "Setting name for 0x%02X with command: '%s'", sensor->config.i2c_address, command);
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, response, sizeof(response), EZO_LONG_WAIT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sensor 0x%02X name set command response: '%s' (len=%d)", 
                 sensor->config.i2c_address, response, strlen(response));
        
        // Wait for command to complete and sensor to be ready
        vTaskDelay(pdMS_TO_TICKS(600));
        
        // Clear any stale data before querying name
        char dummy2[EZO_LARGEST_STRING];
        for (int i = 0; i < 3; i++) {
            ezo_sensor_receive_response(sensor, dummy2, sizeof(dummy2));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Verify by reading back with raw response logging
        char verify[EZO_MAX_SENSOR_NAME] = {0};
        char raw_verify_response[EZO_LARGEST_STRING] = {0};
        esp_err_t verify_ret = ezo_sensor_send_command(sensor, "Name,?", raw_verify_response, 
                                                       sizeof(raw_verify_response), EZO_LONG_WAIT_MS);
        
        ESP_LOGI(TAG, "Sensor 0x%02X name query raw response: '%s' (len=%d, ret=%s)", 
                 sensor->config.i2c_address, raw_verify_response, strlen(raw_verify_response),
                 esp_err_to_name(verify_ret));
        
        // Parse the response
        ezo_sensor_get_name(sensor, verify, sizeof(verify));
        
        if (verify_ret == ESP_OK && strlen(verify) > 0 && strcmp(verify, name) == 0) {
            strncpy(sensor->config.name, name, EZO_MAX_SENSOR_NAME - 1);
            sensor->config.name[EZO_MAX_SENSOR_NAME - 1] = '\0';
            ESP_LOGI(TAG, "✓ Sensor 0x%02X name verified: '%s'", sensor->config.i2c_address, name);
        } else {
            ESP_LOGW(TAG, "⚠ Sensor 0x%02X name verification failed. Set='%s', Read='%s', Raw='%s'", 
                     sensor->config.i2c_address, name, verify, raw_verify_response);
            // Still update local config even if verification fails (sensor may not support name persistence)
            strncpy(sensor->config.name, name, EZO_MAX_SENSOR_NAME - 1);
            sensor->config.name[EZO_MAX_SENSOR_NAME - 1] = '\0';
        }
    } else {
        ESP_LOGE(TAG, "Failed to set sensor name: %s", esp_err_to_name(ret));
    }
    
    // Resume sensor reading only if we paused it
    if (!was_already_paused) {
        sensor_manager_resume_reading();
    }
    
    return ret;
}

/**
 * @brief Get LED status
 */
esp_err_t ezo_sensor_get_led(ezo_sensor_t *sensor, bool *enabled) {
    if (sensor == NULL || enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "L,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?L,<0|1>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?L") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *enabled = (atoi(token) == 1);
        }
    }

    return ESP_OK;
}

/**
 * @brief Set LED control
 */
esp_err_t ezo_sensor_set_led(ezo_sensor_t *sensor, bool enabled) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *command = enabled ? "L,1" : "L,0";
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.led_control = enabled;
    }
    
    return ret;
}

/**
 * @brief Get protocol lock status
 */
esp_err_t ezo_sensor_get_plock(ezo_sensor_t *sensor, bool *locked) {
    if (sensor == NULL || locked == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "Plock,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?Plock,<0|1>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?Plock") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *locked = (atoi(token) == 1);
        }
    }

    return ESP_OK;
}

/**
 * @brief Set protocol lock
 */
esp_err_t ezo_sensor_set_plock(ezo_sensor_t *sensor, bool locked) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *command = locked ? "Plock,1" : "Plock,0";
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.protocol_lock = locked;
    }
    
    return ret;
}

/**
 * @brief Factory reset
 */
esp_err_t ezo_sensor_factory_reset(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "Factory resetting sensor at 0x%02X", sensor->config.i2c_address);
    
    return ezo_sensor_send_command(sensor, "Factory", NULL, 0, EZO_SHORT_WAIT_MS);
}

/**
 * @brief Change I2C address
 */
esp_err_t ezo_sensor_change_i2c_address(ezo_sensor_t *sensor, uint8_t new_address) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[16];
    snprintf(command, sizeof(command), "I2C,%d", new_address);
    
    ESP_LOGW(TAG, "Changing I2C address from 0x%02X to 0x%02X (device will reboot)", 
             sensor->config.i2c_address, new_address);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

// EC-specific functions
esp_err_t ezo_ec_get_probe_type(ezo_sensor_t *sensor, float *probe_type) {
    if (sensor == NULL || probe_type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "K,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?K,<value>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?K") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *probe_type = atof(token);
        }
    }

    return ESP_OK;
}

esp_err_t ezo_ec_set_probe_type(ezo_sensor_t *sensor, float probe_type) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[16];
    snprintf(command, sizeof(command), "K,%.2f", probe_type);
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.ec.probe_type = probe_type;
    }
    
    return ret;
}

esp_err_t ezo_ec_get_tds_factor(ezo_sensor_t *sensor, float *factor) {
    if (sensor == NULL || factor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "TDS,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?TDS,<value>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?TDS") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *factor = atof(token);
        }
    }

    return ESP_OK;
}

esp_err_t ezo_ec_set_tds_factor(ezo_sensor_t *sensor, float factor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[16];
    snprintf(command, sizeof(command), "TDS,%.2f", factor);
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.ec.tds_conversion_factor = factor;
    }
    
    return ret;
}

esp_err_t ezo_ec_set_output_parameter(ezo_sensor_t *sensor, const char *param, bool enabled) {
    if (sensor == NULL || param == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[16];
    snprintf(command, sizeof(command), "O,%s,%d", param, enabled ? 1 : 0);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

// RTD-specific functions
esp_err_t ezo_rtd_get_scale(ezo_sensor_t *sensor, char *scale) {
    if (sensor == NULL || scale == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "S,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?S,<scale>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?S") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL && strlen(token) > 0) {
            *scale = token[0];
        }
    }

    return ESP_OK;
}

esp_err_t ezo_rtd_set_scale(ezo_sensor_t *sensor, char scale) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[8];
    snprintf(command, sizeof(command), "S,%c", scale);
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.rtd.temperature_scale = scale;
    }
    
    return ret;
}

// pH-specific functions
esp_err_t ezo_ph_get_extended_scale(ezo_sensor_t *sensor, bool *enabled) {
    if (sensor == NULL || enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[EZO_LARGEST_STRING] = {0};
    
    esp_err_t ret = ezo_sensor_send_command(sensor, "pHext,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse response: ?pHext,<0|1>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?pHext") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *enabled = (atoi(token) == 1);
        }
    }

    return ESP_OK;
}

esp_err_t ezo_ph_set_extended_scale(ezo_sensor_t *sensor, bool enabled) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[16];
    snprintf(command, sizeof(command), "pHext,%d", enabled ? 1 : 0);
    
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.ph.extended_scale = enabled;
    }
    
    return ret;
}

// Calibration functions
esp_err_t ezo_ph_calibrate(ezo_sensor_t *sensor, const char *point, float value) {
    if (sensor == NULL || point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    
    if (strcmp(point, "clear") == 0) {
        snprintf(command, sizeof(command), "Cal,clear");
    } else if (strcmp(point, "mid") == 0) {
        snprintf(command, sizeof(command), "Cal,mid,%.2f", value);
    } else if (strcmp(point, "low") == 0) {
        snprintf(command, sizeof(command), "Cal,low,%.2f", value);
    } else if (strcmp(point, "high") == 0) {
        snprintf(command, sizeof(command), "Cal,high,%.2f", value);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_rtd_calibrate(ezo_sensor_t *sensor, float temperature) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    
    if (temperature <= -999.0f) {
        snprintf(command, sizeof(command), "Cal,clear");
    } else {
        snprintf(command, sizeof(command), "Cal,%.2f", temperature);
    }
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ec_calibrate(ezo_sensor_t *sensor, const char *point, uint32_t value) {
    if (sensor == NULL || point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    
    if (strcmp(point, "clear") == 0) {
        snprintf(command, sizeof(command), "Cal,clear");
    } else if (strcmp(point, "dry") == 0) {
        snprintf(command, sizeof(command), "Cal,dry");
    } else if (strcmp(point, "low") == 0) {
        snprintf(command, sizeof(command), "Cal,low,%lu", (unsigned long)value);
    } else if (strcmp(point, "high") == 0) {
        snprintf(command, sizeof(command), "Cal,high,%lu", (unsigned long)value);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_do_calibrate(ezo_sensor_t *sensor, const char *point) {
    if (sensor == NULL || point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    
    if (strcmp(point, "clear") == 0) {
        snprintf(command, sizeof(command), "Cal,clear");
    } else if (strcmp(point, "atm") == 0) {
        snprintf(command, sizeof(command), "Cal");
    } else if (strcmp(point, "0") == 0) {
        snprintf(command, sizeof(command), "Cal,0");
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_orp_calibrate(ezo_sensor_t *sensor, float value) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    
    if (value <= -999.0f) {
        snprintf(command, sizeof(command), "Cal,clear");
    } else {
        snprintf(command, sizeof(command), "Cal,%.0f", value);
    }
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_get_calibration_status(ezo_sensor_t *sensor, char *status, size_t status_size) {
    if (sensor == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "Cal,?", status, status_size, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ph_get_temperature_comp(ezo_sensor_t *sensor, float *temperature_c) {
    if (sensor == NULL || temperature_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_TEMP_COMP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_send_command(sensor, "T,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?T") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *temperature_c = atof(token);
            sensor->config.temp_compensation = *temperature_c;
            sensor->config.temp_comp_valid = true;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t ezo_ph_set_temperature_comp(ezo_sensor_t *sensor, float temperature_c) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_TEMP_COMP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char command[16];
    snprintf(command, sizeof(command), "T,%.2f", temperature_c);
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.temp_compensation = temperature_c;
        sensor->config.temp_comp_valid = true;
    }
    return ret;
}

esp_err_t ezo_sensor_set_continuous_mode(ezo_sensor_t *sensor, bool enable) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_MODE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *command = enable ? "C" : "C,0";
    esp_err_t ret = ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.continuous_mode = enable;
    }
    return ret;
}

esp_err_t ezo_sensor_get_continuous_mode(ezo_sensor_t *sensor, bool *enabled) {
    if (sensor == NULL || enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_MODE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_send_command(sensor, "C,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?C") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *enabled = (atoi(token) == 1);
            sensor->config.continuous_mode = *enabled;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t ezo_sensor_sleep(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_SLEEP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = ezo_sensor_send_command(sensor, "Sleep", NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.sleeping = true;
    }
    return ret;
}

esp_err_t ezo_sensor_wake(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(sensor->config.capability_flags & EZO_CAP_SLEEP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = ezo_sensor_send_command(sensor, "Wake", NULL, 0, EZO_SHORT_WAIT_MS);
    if (ret == ESP_OK) {
        sensor->config.sleeping = false;
    }
    return ret;
}

// Output string control functions
esp_err_t ezo_rtd_set_output_parameter(ezo_sensor_t *sensor, const char *param, bool enabled) {
    if (sensor == NULL || param == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    snprintf(command, sizeof(command), "O,%s,%d", param, enabled ? 1 : 0);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ph_set_output_parameter(ezo_sensor_t *sensor, const char *param, bool enabled) {
    if (sensor == NULL || param == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    snprintf(command, sizeof(command), "O,%s,%d", param, enabled ? 1 : 0);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_do_set_output_parameter(ezo_sensor_t *sensor, const char *param, bool enabled) {
    if (sensor == NULL || param == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char command[32];
    snprintf(command, sizeof(command), "O,%s,%d", param, enabled ? 1 : 0);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_get_output_config(ezo_sensor_t *sensor, char *config, size_t config_size) {
    if (sensor == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "O,?", config, config_size, EZO_SHORT_WAIT_MS);
}

// Advanced features implementation
esp_err_t ezo_sensor_export_calibration(ezo_sensor_t *sensor, char *export_data, size_t data_size) {
    if (sensor == NULL || export_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "Export", export_data, data_size, EZO_LONG_WAIT_MS);
}

esp_err_t ezo_sensor_import_calibration(ezo_sensor_t *sensor, const char *import_data) {
    if (sensor == NULL || import_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char command[128];
    snprintf(command, sizeof(command), "Import,%s", import_data);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_LONG_WAIT_MS);
}

esp_err_t ezo_ph_get_slope(ezo_sensor_t *sensor, char *slope_data, size_t data_size) {
    if (sensor == NULL || slope_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_PH) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    return ezo_sensor_send_command(sensor, "Slope,?", slope_data, data_size, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_find(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "Find", NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_get_status(ezo_sensor_t *sensor, char *status_data, size_t data_size) {
    if (sensor == NULL || status_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "Status", status_data, data_size, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_get_baud(ezo_sensor_t *sensor, uint32_t *baud_rate) {
    if (sensor == NULL || baud_rate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_send_command(sensor, "Baud,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Parse response: ?Baud,<value>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?Baud") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *baud_rate = (uint32_t)atoi(token);
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

esp_err_t ezo_sensor_set_baud(ezo_sensor_t *sensor, uint32_t baud_rate) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate baud rate
    if (baud_rate < 300 || baud_rate > 115200) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char command[32];
    snprintf(command, sizeof(command), "Baud,%lu", (unsigned long)baud_rate);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

// EC-specific advanced functions
esp_err_t ezo_ec_get_temperature_comp(ezo_sensor_t *sensor, float *temperature_c) {
    if (sensor == NULL || temperature_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_send_command(sensor, "T,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?T") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *temperature_c = atof(token);
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

esp_err_t ezo_ec_set_temperature_comp(ezo_sensor_t *sensor, float temperature_c) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    char command[32];
    snprintf(command, sizeof(command), "T,%.2f", temperature_c);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ec_get_data_logger_interval(ezo_sensor_t *sensor, uint32_t *interval_ms) {
    if (sensor == NULL || interval_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    char response[EZO_LARGEST_STRING] = {0};
    esp_err_t ret = ezo_sensor_send_command(sensor, "D,?", response, sizeof(response), EZO_SHORT_WAIT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Parse response: ?D,<value>
    char *token = strtok(response, ",");
    if (token != NULL && strcmp(token, "?D") == 0) {
        token = strtok(NULL, ",");
        if (token != NULL) {
            *interval_ms = (uint32_t)atoi(token);
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

esp_err_t ezo_ec_set_data_logger_interval(ezo_sensor_t *sensor, uint32_t interval_ms) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    char command[32];
    snprintf(command, sizeof(command), "D,%lu", (unsigned long)interval_ms);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ec_set_k_lock(ezo_sensor_t *sensor, bool locked) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    const char *command = locked ? "K,lock" : "K,unlock";
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_ec_set_tds_lock(ezo_sensor_t *sensor, bool locked) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_EC) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    const char *command = locked ? "TDS,lock" : "TDS,unlock";
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_hum_set_output_parameter(ezo_sensor_t *sensor, const char *param, bool enabled) {
    if (sensor == NULL || param == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strcmp(sensor->config.type, EZO_TYPE_HUM) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    char command[32];
    snprintf(command, sizeof(command), "O,%s,%d", param, enabled ? 1 : 0);
    
    return ezo_sensor_send_command(sensor, command, NULL, 0, EZO_SHORT_WAIT_MS);
}

esp_err_t ezo_sensor_memory_clear(ezo_sensor_t *sensor) {
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ezo_sensor_send_command(sensor, "M,clear", NULL, 0, EZO_SHORT_WAIT_MS);
}
