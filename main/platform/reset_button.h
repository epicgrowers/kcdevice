/**
 * @file reset_button.h
 * @brief GPIO button handler for clearing WiFi credentials and factory reset
 * 
 * Provides button press detection with debouncing:
 * - Short press (< 3s): Clear WiFi credentials and restart provisioning
 * - Long press (≥ 3s): Factory reset (clear all NVS data)
 */

#ifndef RESET_BUTTON_H
#define RESET_BUTTON_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Default GPIO pin for reset button
 * ESP32-S3 (Adafruit Metro): A0 = GPIO14
 * ESP32-C6 (Seeed XIAO with Expansion Board): D1 = GPIO1 (built-in user button)
 */
#ifdef CONFIG_IDF_TARGET_ESP32C6
#define RESET_BUTTON_GPIO 1  // D1 (built-in user button on Seeed XIAO Expansion Board)
#else
#define RESET_BUTTON_GPIO 14 // A0 on Adafruit Metro ESP32-S3
#endif

/**
 * @brief Button press duration thresholds (milliseconds)
 */
#define RESET_BUTTON_SHORT_PRESS_MS 100    // Minimum for valid press
#define RESET_BUTTON_LONG_PRESS_MS  3000   // Threshold for factory reset

/**
 * @brief Button event types
 */
typedef enum {
    RESET_BUTTON_EVENT_SHORT_PRESS,  /*!< Short press detected (clear WiFi) */
    RESET_BUTTON_EVENT_LONG_PRESS,   /*!< Long press detected (factory reset) */
} reset_button_event_t;

/**
 * @brief Button event callback function type
 * 
 * @param event The button event that occurred
 * @param press_duration_ms Duration of button press in milliseconds
 */
typedef void (*reset_button_callback_t)(reset_button_event_t event, uint32_t press_duration_ms);

/**
 * @brief Initialize reset button GPIO and interrupt handler
 * 
 * Configures the specified GPIO pin with internal pull-up and sets up
 * interrupt-based button press detection with debouncing.
 * 
 * @param gpio_num GPIO pin number for the button (use RESET_BUTTON_GPIO for default)
 * @param callback Callback function to handle button events
 * @return 
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if callback is NULL
 *     - ESP_FAIL if GPIO configuration fails
 */
esp_err_t reset_button_init(int gpio_num, reset_button_callback_t callback);

/**
 * @brief Check if button is currently pressed
 * 
 * @return true if button is pressed, false otherwise
 */
bool reset_button_is_pressed(void);

/**
 * @brief Deinitialize reset button and free resources
 * 
 * @return ESP_OK on success
 */
esp_err_t reset_button_deinit(void);

#endif // RESET_BUTTON_H
