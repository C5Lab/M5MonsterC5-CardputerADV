/**
 * @file main.c
 * @brief Main entry point for Cardputer-ADV WiFi Attack application
 */

#include <stdio.h>
#include <string.h>
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
#include "text_ui.h"

#include "version.h"

// Screen timeout is now configurable via Settings (stored in NVS)

static const char *TAG = "MAIN";

static volatile bool board_sd_missing = false;
static volatile bool board_sd_check_pending = false;
static volatile bool board_sd_ok_received = false;
static int64_t board_sd_check_start_ms = 0;
static bool board_sd_popup_shown = false;

#define SD_CHECK_TIMEOUT_MS   3000
#define SD_CHECK_RETRIES      2
#define SD_CHECK_BOOT_DELAY_MS 1000

bool is_board_sd_missing(void)
{
    return board_sd_missing;
}

static void uart_sd_check_line_callback(const char *line, void *user_data)
{
    (void)user_data;
    if (!line || !board_sd_check_pending) {
        return;
    }

    if (strcmp(line, "SD_OK") == 0) {
        board_sd_ok_received = true;
        board_sd_check_pending = false;
    } else if (strcmp(line, "SD_NONE") == 0) {
        board_sd_missing = true;
        board_sd_check_pending = false;
    }
}

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

    // Apply saved brightness setting
    display_set_backlight(settings_get_screen_brightness());

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
    bool sd_card_missing = false;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Screenshot module initialization failed - screenshots disabled");
        sd_card_missing = true;
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

    // SD card warning popup (after keyboard init so ESC works)
    if (sd_card_missing) {
        ESP_LOGW(TAG, "SD card not detected, showing popup...");
        ui_clear();
        ui_show_message("Warning", "SD card not detected");
        display_flush();

        // Wait up to 2 seconds or until ESC is pressed
        for (int i = 0; i < 200; i++) {
            keyboard_process();
            if (keyboard_get_key() == KEY_ESC) {
                ESP_LOGI(TAG, "SD card warning popup dismissed by user");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        ui_clear();
    }

    // Initialize UART handler
    ESP_LOGI(TAG, "Initializing UART handler...");
    ret = uart_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART handler initialization failed!");
        return;
    }
    ESP_LOGI(TAG, "UART handler initialized successfully");
    uart_register_monitor_callback(uart_sd_check_line_callback, NULL);

    // Board detection - check if ESP32C5 board is connected
    ESP_LOGI(TAG, "Checking for ESP32C5 board...");
    bool board_detected = uart_check_board_ping(500);
    bool popup_dismissed = false;

    if (!board_detected) {
        ESP_LOGW(TAG, "Board not detected, showing popup...");
        
        // Show popup
        ui_clear();
        ui_show_message("Warning", "Board not detected");
        display_flush();
        
        // Retry loop - ping every 1s until board detected or ESC pressed
        while (!board_detected && !popup_dismissed) {
            // Check for ESC key to dismiss popup
            // Poll keyboard for ~1 second in small intervals
            for (int i = 0; i < 100 && !popup_dismissed; i++) {
                keyboard_process();
                key_code_t key = keyboard_get_key();
                if (key == KEY_ESC) {
                    popup_dismissed = true;
                    ESP_LOGI(TAG, "Board detection popup dismissed by user");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            // If not dismissed, try ping again
            if (!popup_dismissed) {
                board_detected = uart_check_board_ping(500);
            }
        }
        
        // Clear popup
        ui_clear();
    }
    
    if (board_detected) {
        ESP_LOGI(TAG, "ESP32C5 board detected");

        // Give Monster time to finish SD mount after responding to ping
        ESP_LOGI(TAG, "Waiting %dms for Monster SD init...", SD_CHECK_BOOT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(SD_CHECK_BOOT_DELAY_MS));

        // Retry sd_status up to SD_CHECK_RETRIES times
        for (int attempt = 1; attempt <= SD_CHECK_RETRIES; attempt++) {
            ESP_LOGI(TAG, "SD check attempt %d/%d...", attempt, SD_CHECK_RETRIES);
            board_sd_ok_received = false;
            board_sd_missing = false;
            board_sd_check_pending = true;
            board_sd_check_start_ms = esp_timer_get_time() / 1000;
            uart_send_command("sd_status");

            // Block-wait for response (up to SD_CHECK_TIMEOUT_MS per attempt)
            while (board_sd_check_pending) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - board_sd_check_start_ms) > SD_CHECK_TIMEOUT_MS) {
                    board_sd_check_pending = false;
                    ESP_LOGW(TAG, "SD check attempt %d timed out", attempt);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (board_sd_ok_received) {
                ESP_LOGI(TAG, "Monster SD OK on attempt %d", attempt);
                break;
            } else if (board_sd_missing) {
                ESP_LOGW(TAG, "Monster SD missing confirmed on attempt %d", attempt);
                break;
            }

            // No response — retry after short pause (except on last attempt)
            if (attempt < SD_CHECK_RETRIES) {
                ESP_LOGW(TAG, "No SD response, retrying in 500ms...");
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        if (!board_sd_ok_received && !board_sd_missing) {
            ESP_LOGW(TAG, "Monster SD status unknown after all attempts");
        }
    } else {
        ESP_LOGW(TAG, "Continuing without board detection");
    }

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
                display_set_backlight(settings_get_screen_brightness());
                screen_dimmed = false;
                ESP_LOGI(TAG, "Screen woken by keypress");
            }
        }

        // SD card missing: show warning and allow ESC to continue
        if (board_sd_missing && !board_sd_popup_shown) {
            ESP_LOGW(TAG, "Board SD card not detected, showing popup...");
            display_set_backlight(settings_get_screen_brightness());
            ui_clear();
            ui_show_message_tall("Warning",
                            "SD missing in MonsterC5\n"
                            "Insert SD and Off/On\n"
                            "Press Esc to skip\n"
                            "Some functions limited");
            display_flush();

            // Block UI input until ESC (no arrow/menu handling under popup)
            keyboard_set_callback_enabled(false);
            while (true) {
                keyboard_process();
                bool esc_pressed = false;
                key_code_t key = KEY_NONE;
                while ((key = keyboard_get_key()) != KEY_NONE) {
                    if (key == KEY_ESC) {
                        esc_pressed = true;
                    }
                }
                if (esc_pressed) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            keyboard_set_callback_enabled(true);
            ui_clear();
            screen_manager_redraw();
            board_sd_popup_shown = true;
        }

        // Check for screen timeout (0 = stays on, never dims)
        uint32_t timeout = settings_get_screen_timeout_ms();
        int64_t now = esp_timer_get_time() / 1000;
        if (!screen_dimmed && timeout > 0 && (now - last_activity_time) > timeout) {
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
