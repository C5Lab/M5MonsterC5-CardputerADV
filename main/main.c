/**
 * @file main.c
 * @brief Main entry point for Cardputer-ADV WiFi Attack application
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "display.h"
#include "keyboard.h"
#include "uart_handler.h"
#include "screen_manager.h"
#include "home_screen.h"
#include "screenshot.h"
#include "battery.h"
#include "settings.h"
#include "buzzer.h"

#define JANOS_ADV_VERSION "1.3.5"

// Screen timeout configuration
#define SCREEN_TIMEOUT_MS  30000  // 30 seconds

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Cardputer-ADV WiFi Attack Application Starting...");

    // Initialize settings (NVS)
    ESP_LOGI(TAG, "Initializing settings...");
    esp_err_t ret = settings_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Settings initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "Settings initialized successfully");

    // Initialize display
    ESP_LOGI(TAG, "Initializing display...");
    ret = display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "Display initialized successfully");

    // Initialize battery monitoring
    ESP_LOGI(TAG, "Initializing battery monitoring...");
    ret = battery_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery monitoring initialization failed - battery indicator disabled");
        // Continue anyway - battery monitoring is optional
    } else {
        ESP_LOGI(TAG, "Battery monitoring initialized successfully");
    }

    // Initialize screenshot module (SD card)
    ESP_LOGI(TAG, "Initializing screenshot module...");
    ret = screenshot_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Screenshot module initialization failed - screenshots disabled");
        // Continue anyway - screenshots are optional
    } else {
        ESP_LOGI(TAG, "Screenshot module initialized successfully");
    }

    // Initialize keyboard
    ESP_LOGI(TAG, "Initializing keyboard...");
    ret = keyboard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "Keyboard initialized successfully");

    // Initialize UART handler
    ESP_LOGI(TAG, "Initializing UART handler...");
    ret = uart_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART handler initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "UART handler initialized successfully");

    // Initialize buzzer
    ESP_LOGI(TAG, "Initializing buzzer...");
    ret = buzzer_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer initialization failed - audio disabled");
        // Continue anyway - buzzer is optional
    } else {
        ESP_LOGI(TAG, "Buzzer initialized successfully");
    }

    // Initialize screen manager
    ESP_LOGI(TAG, "Initializing screen manager...");
    screen_manager_init();
    ESP_LOGI(TAG, "Screen manager initialized successfully");

    // Push home screen as the initial screen
    ESP_LOGI(TAG, "Loading home screen...");
    screen_manager_push(home_screen_create, NULL);

    ESP_LOGI(TAG, "Application started successfully!");

    // Main loop - process keyboard input and periodic screen updates
    int tick_counter = 0;
    bool screen_dimmed = false;
    int64_t last_activity_time = esp_timer_get_time() / 1000;  // Convert to ms
    
    while (1) {
        keyboard_process();
        
        // Check for any key activity
        key_code_t key = keyboard_get_key();
        if (key != KEY_NONE) {
            // Reset activity timer on any keypress
            last_activity_time = esp_timer_get_time() / 1000;
            
            // Wake screen if dimmed
            if (screen_dimmed) {
                display_set_backlight(100);
                screen_dimmed = false;
                ESP_LOGI(TAG, "Screen woken by keypress");
            }
        }
        
        // Check for screen timeout
        int64_t now = esp_timer_get_time() / 1000;
        if (!screen_dimmed && (now - last_activity_time) > SCREEN_TIMEOUT_MS) {
            display_set_backlight(0);
            screen_dimmed = true;
            ESP_LOGI(TAG, "Screen dimmed due to inactivity");
        }
        
        // Call screen tick every 500ms (50 * 10ms) for responsive UI updates
        if (++tick_counter >= 50) {
            tick_counter = 0;
            screen_manager_tick();
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
