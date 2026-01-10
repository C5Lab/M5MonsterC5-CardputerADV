/**
 * @file uart_pins_screen.c
 * @brief UART pins configuration screen implementation
 */

#include "uart_pins_screen.h"
#include "settings.h"
#include "text_ui.h"
#include "keyboard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "UART_PINS_SCREEN";

// Predefined UART pin configurations
typedef struct {
    const char *name;
    int tx_pin;
    int rx_pin;
} uart_config_option_t;

static const uart_config_option_t uart_options[] = {
    {"Monster Cap",   15, 13},
    {"Monster Grove",  2,  1},
};

#define OPTION_COUNT (sizeof(uart_options) / sizeof(uart_options[0]))

// Screen user data
typedef struct {
    int selected_index;
    int current_option;  // Currently saved option (-1 if custom)
    bool saved;
} uart_pins_data_t;

// Determine which option matches current settings (-1 if none)
static int get_current_option(void)
{
    int tx = settings_get_uart_tx_pin();
    int rx = settings_get_uart_rx_pin();
    
    for (int i = 0; i < (int)OPTION_COUNT; i++) {
        if (uart_options[i].tx_pin == tx && uart_options[i].rx_pin == rx) {
            return i;
        }
    }
    return -1;  // Custom configuration
}

static void draw_screen(screen_t *self)
{
    uart_pins_data_t *data = (uart_pins_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("UART Pins");
    
    // Draw options with checkboxes
    for (int i = 0; i < (int)OPTION_COUNT; i++) {
        char label[32];
        snprintf(label, sizeof(label), "%s (%d/%d)", 
                 uart_options[i].name,
                 uart_options[i].tx_pin,
                 uart_options[i].rx_pin);
        
        bool is_selected = (i == data->selected_index);
        bool is_current = (i == data->current_option);
        
        ui_draw_menu_item(i + 1, label, is_selected, true, is_current);
    }
    
    // Show saved message
    if (data->saved) {
        ui_print(0, 5, "Saved! Restart required.", UI_COLOR_TITLE);
    }
    
    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

// Redraw a single menu row
static void redraw_row(uart_pins_data_t *data, int idx)
{
    if (idx < 0 || idx >= (int)OPTION_COUNT) return;
    
    char label[32];
    snprintf(label, sizeof(label), "%s (%d/%d)", 
             uart_options[idx].name,
             uart_options[idx].tx_pin,
             uart_options[idx].rx_pin);
    
    bool is_selected = (idx == data->selected_index);
    bool is_current = (idx == data->current_option);
    
    ui_draw_menu_item(idx + 1, label, is_selected, true, is_current);
}

static void on_key(screen_t *self, key_code_t key)
{
    uart_pins_data_t *data = (uart_pins_data_t *)self->user_data;
    
    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                data->selected_index--;
                // Clear "Saved!" message if shown
                if (data->saved) {
                    data->saved = false;
                    display_fill_rect(0, 5 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
                }
                // Redraw only 2 rows
                redraw_row(data, old_idx);
                redraw_row(data, data->selected_index);
            }
            break;
            
        case KEY_DOWN:
            if (data->selected_index < (int)OPTION_COUNT - 1) {
                int old_idx = data->selected_index;
                data->selected_index++;
                // Clear "Saved!" message if shown
                if (data->saved) {
                    data->saved = false;
                    display_fill_rect(0, 5 * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
                }
                // Redraw only 2 rows
                redraw_row(data, old_idx);
                redraw_row(data, data->selected_index);
            }
            break;
            
        case KEY_ENTER:
        case KEY_SPACE:
            {
                const uart_config_option_t *opt = &uart_options[data->selected_index];
                esp_err_t ret = settings_set_uart_pins(opt->tx_pin, opt->rx_pin);
                if (ret == ESP_OK) {
                    data->current_option = data->selected_index;
                    data->saved = true;
                }
                draw_screen(self);
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    uart_pins_data_t *data = (uart_pins_data_t *)self->user_data;
    data->current_option = get_current_option();
    draw_screen(self);
}

screen_t* uart_pins_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating UART pins screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    uart_pins_data_t *data = calloc(1, sizeof(uart_pins_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    // Determine current option
    data->current_option = get_current_option();
    data->selected_index = (data->current_option >= 0) ? data->current_option : 0;
    data->saved = false;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "UART pins screen created");
    return screen;
}
