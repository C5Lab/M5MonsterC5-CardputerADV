/**
 * @file blackout_screen.c
 * @brief Blackout attack screen implementation
 */

#include "blackout_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "BLACKOUT";

static void draw_screen(screen_t *self)
{
    (void)self;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Blackout");
    
    // Draw status message centered
    ui_print_center(3, "Blackout ongoing", UI_COLOR_HIGHLIGHT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop & Exit");
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            // Send stop command before exiting
            uart_send_command("stop");
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    // Nothing to free - no user data allocated
    (void)self;
}

screen_t* blackout_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating blackout screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    screen->user_data = NULL;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Send start_blackout command
    uart_send_command("start_blackout");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Blackout screen created, attack started");
    return screen;
}



