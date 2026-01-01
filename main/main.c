/**
 * @file main.c
 * @brief Main entry point for Cardputer-ADV WiFi Attack application
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "display.h"
#include "keyboard.h"
#include "uart_handler.h"
#include "screen_manager.h"
#include "home_screen.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Cardputer-ADV WiFi Attack Application Starting...");

    // Initialize display
    ESP_LOGI(TAG, "Initializing display...");
    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "Display initialized successfully");

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
    while (1) {
        keyboard_process();
        
        // Call screen tick every 500ms (50 * 10ms) for responsive UI updates
        if (++tick_counter >= 50) {
            tick_counter = 0;
            screen_manager_tick();
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
