/**
 * @file reset_button.c
 * @brief GPIO button handler implementation for WiFi reset
 */

#include "reset_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "RESET_BTN";

// Event structure for passing to handler task
typedef struct {
    reset_button_event_t event;
    uint32_t press_duration_ms;
} button_event_msg_t;

// Button state tracking
static int button_gpio = -1;
static reset_button_callback_t button_callback = NULL;
static int64_t button_press_start_time = 0;
static bool button_pressed = false;
static bool event_fired = false;
static QueueHandle_t button_event_queue = NULL;

/**
 * @brief GPIO interrupt handler for button events
 */
static void IRAM_ATTR reset_button_isr_handler(void* arg)
{
    // Read current button state (active low with pull-up)
    int level = gpio_get_level(button_gpio);
    int64_t now = esp_timer_get_time();
    
    if (level == 0) {
        // Button pressed (low state)
        if (!button_pressed) {
            button_pressed = true;
            button_press_start_time = now;
            event_fired = false;
        }
    } else {
        // Button released (high state)
        if (button_pressed) {
            button_pressed = false;
            
            // Calculate press duration
            uint32_t press_duration_ms = (now - button_press_start_time) / 1000;
            
            // Fire event if not already fired and meets minimum duration
            if (!event_fired && press_duration_ms >= RESET_BUTTON_SHORT_PRESS_MS) {
                reset_button_event_t event;
                
                if (press_duration_ms >= RESET_BUTTON_LONG_PRESS_MS) {
                    event = RESET_BUTTON_EVENT_LONG_PRESS;
                } else {
                    event = RESET_BUTTON_EVENT_SHORT_PRESS;
                }
                
                // Post event to queue (ISR-safe)
                if (button_event_queue != NULL) {
                    button_event_msg_t msg = {
                        .event = event,
                        .press_duration_ms = press_duration_ms
                    };
                    BaseType_t higher_priority_task_woken = pdFALSE;
                    xQueueSendFromISR(button_event_queue, &msg, &higher_priority_task_woken);
                    if (higher_priority_task_woken == pdTRUE) {
                        portYIELD_FROM_ISR();
                    }
                }
                
                event_fired = true;
            }
        }
    }
}

/**
 * @brief Task to handle button events from queue (safe context for callbacks)
 */
static void reset_button_event_handler_task(void* arg)
{
    button_event_msg_t msg;
    
    while (1) {
        // Wait for events from the queue
        if (xQueueReceive(button_event_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Call the callback in task context (safe for logging, WiFi ops, etc.)
            if (button_callback != NULL) {
                button_callback(msg.event, msg.press_duration_ms);
            }
        }
    }
}

/**
 * @brief Task to monitor long press and fire event while button is held
 */
static void reset_button_monitor_task(void* arg)
{
    while (1) {
        if (button_pressed && !event_fired) {
            int64_t now = esp_timer_get_time();
            uint32_t press_duration_ms = (now - button_press_start_time) / 1000;
            
            ESP_LOGI(TAG, "Button held for %lu ms...", (unsigned long)press_duration_ms);
            
            // Fire long press event if threshold reached
            if (press_duration_ms >= RESET_BUTTON_LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Long press detected (%lu ms)", (unsigned long)press_duration_ms);
                
                // Post to queue instead of calling callback directly
                if (button_event_queue != NULL) {
                    button_event_msg_t msg = {
                        .event = RESET_BUTTON_EVENT_LONG_PRESS,
                        .press_duration_ms = press_duration_ms
                    };
                    xQueueSend(button_event_queue, &msg, 0);
                }
                
                event_fired = true;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
    }
}

esp_err_t reset_button_init(int gpio_num, reset_button_callback_t callback)
{
    if (callback == NULL) {
        ESP_LOGE(TAG, "Callback function cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    button_gpio = gpio_num;
    button_callback = callback;
    
    ESP_LOGI(TAG, "Initializing reset button on GPIO%d", gpio_num);
    
    // Create event queue for passing events from ISR to handler task
    button_event_queue = xQueueCreate(5, sizeof(button_event_msg_t));
    if (button_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_FAIL;
    }
    
    // Configure GPIO as input with pull-up (button connects to GND when pressed)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE, // Trigger on both rising and falling edge
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        vQueueDelete(button_event_queue);
        return ret;
    }
    
    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means service already installed (OK)
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
        vQueueDelete(button_event_queue);
        return ret;
    }
    
    // Attach ISR handler to GPIO
    ret = gpio_isr_handler_add(gpio_num, reset_button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        vQueueDelete(button_event_queue);
        return ret;
    }
    
    // Create event handler task (runs callbacks in safe task context)
    BaseType_t task_ret = xTaskCreate(
        reset_button_event_handler_task,
        "reset_btn_handler",
        4096,  // Increased stack for WiFi operations
        NULL,
        6,     // Higher priority than monitor task
        NULL
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create event handler task");
        gpio_isr_handler_remove(gpio_num);
        vQueueDelete(button_event_queue);
        return ESP_FAIL;
    }
    
    // Create monitoring task for long press detection
    task_ret = xTaskCreate(
        reset_button_monitor_task,
        "reset_btn_monitor",
        2048,
        NULL,
        5,
        NULL
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        gpio_isr_handler_remove(gpio_num);
        vQueueDelete(button_event_queue);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Reset button initialized successfully");
    ESP_LOGI(TAG, "  Short press (<%ds): Clear WiFi credentials", RESET_BUTTON_LONG_PRESS_MS/1000);
    ESP_LOGI(TAG, "  Long press (>=%ds): Factory reset", RESET_BUTTON_LONG_PRESS_MS/1000);
    
    return ESP_OK;
}

bool reset_button_is_pressed(void)
{
    if (button_gpio < 0) {
        return false;
    }
    
    // Button is active low (returns 0 when pressed)
    return (gpio_get_level(button_gpio) == 0);
}

esp_err_t reset_button_deinit(void)
{
    if (button_gpio < 0) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing reset button");
    
    gpio_isr_handler_remove(button_gpio);
    button_gpio = -1;
    button_callback = NULL;
    
    return ESP_OK;
}
