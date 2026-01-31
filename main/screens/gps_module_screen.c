/**
 * @file gps_module_screen.c
 * @brief GPS module configuration screen implementation
 */

#include "gps_module_screen.h"
#include "gps_raw_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "keyboard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "GPS_MODULE_SCREEN";

// Options
static const char *options[] = {
    "ATGM336H",
    "M5Stack GPS 1.1",
};

#define OPTION_ATGM 0
#define OPTION_M5   1
#define OPTION_COUNT 2

#define MENU_GPS_READ 2
#define MENU_ITEM_COUNT 3

// Screen user data
typedef struct {
    int selected_index;
    int current_option;  // 0 = atgm, 1 = m5, -1 = unknown
    bool loading;
    bool saved;
    bool needs_redraw;   // Flag for deferred redraw from UART callback
    char status_msg[32];
} gps_module_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

static int parse_current_module(const char *line)
{
    if (strstr(line, "Current GPS module:") == NULL) {
        return -1;
    }
    if (strstr(line, "ATGM") != NULL) {
        return OPTION_ATGM;
    }
    if (strstr(line, "M5StackGPS") != NULL || strstr(line, "M5") != NULL) {
        return OPTION_M5;
    }
    return -1;
}

// UART response callback - runs in UART RX task context, DO NOT call display functions!
static void on_uart_response(const char *line, void *user_data)
{
    screen_t *self = (screen_t *)user_data;
    gps_module_data_t *data = (gps_module_data_t *)self->user_data;

    int parsed = parse_current_module(line);
    if (parsed >= 0) {
        data->current_option = parsed;
        data->selected_index = parsed;
        data->loading = false;
        data->status_msg[0] = '\0';

        uart_clear_line_callback();
        data->needs_redraw = true;  // Will be handled by on_tick
    }
}

// Periodic tick handler - runs in main task context
static void on_tick(screen_t *self)
{
    gps_module_data_t *data = (gps_module_data_t *)self->user_data;

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void draw_screen(screen_t *self)
{
    gps_module_data_t *data = (gps_module_data_t *)self->user_data;

    ui_clear();

    // Draw title
    ui_draw_title("GPS");

    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else {
        // Draw options with checkboxes
        for (int i = 0; i < OPTION_COUNT; i++) {
            bool is_selected = (i == data->selected_index);
            bool is_current = (i == data->current_option);
            
            ui_draw_menu_item(i + 1, options[i], is_selected, true, is_current);
        }

        ui_draw_menu_item(MENU_GPS_READ + 1, "GPS Read", data->selected_index == MENU_GPS_READ, false, false);

        // Show status message
        if (data->status_msg[0]) {
            ui_print(0, 5, data->status_msg, UI_COLOR_TITLE);
        } else if (data->saved) {
            ui_print(0, 5, "Saved!", UI_COLOR_TITLE);
        }
    }

    // Draw status bar
    ui_draw_status("UP/DOWN:Nav ENTER:Select ESC:Back");
}

static void on_key(screen_t *self, key_code_t key)
{
    gps_module_data_t *data = (gps_module_data_t *)self->user_data;

    // Ignore keys while loading
    if (data->loading) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            uart_clear_line_callback();
            screen_manager_pop();
        }
        return;
    }

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                data->selected_index--;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < MENU_ITEM_COUNT - 1) {
                data->selected_index++;
                data->saved = false;
                data->status_msg[0] = '\0';
                draw_screen(self);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            {
                if (data->selected_index == MENU_GPS_READ) {
                    screen_manager_push(gps_raw_screen_create, NULL);
                    return;
                } else {
                    const char *cmd = (data->selected_index == OPTION_ATGM) ? "gps_set atgm" : "gps_set m5";
                    ESP_LOGI(TAG, "Sending: %s", cmd);
                    
                    esp_err_t ret = uart_send_command(cmd);
                    if (ret == ESP_OK) {
                        data->current_option = data->selected_index;
                        data->saved = true;
                        data->status_msg[0] = '\0';
                    } else {
                        snprintf(data->status_msg, sizeof(data->status_msg), "Send failed!");
                    }
                }
                draw_screen(self);
            }
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            uart_clear_line_callback();
            screen_manager_pop();
            break;

        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    gps_module_data_t *data = (gps_module_data_t *)self->user_data;

    data->loading = true;
    data->saved = false;
    data->needs_redraw = false;
    data->status_msg[0] = '\0';

    uart_register_line_callback(on_uart_response, self);
    uart_send_command("gps_set");

    draw_screen(self);
}

screen_t* gps_module_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating GPS module screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    // Allocate user data
    gps_module_data_t *data = calloc(1, sizeof(gps_module_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->selected_index = 0;
    data->current_option = -1;  // Unknown until we read
    data->loading = true;
    data->saved = false;
    data->needs_redraw = false;
    data->status_msg[0] = '\0';

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(on_uart_response, screen);
    uart_send_command("gps_set");

    draw_screen(screen);

    ESP_LOGI(TAG, "GPS module screen created");
    return screen;
}
