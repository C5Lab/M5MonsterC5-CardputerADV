/**
 * @file settings_screen.c
 * @brief Settings menu screen implementation
 */

#include "settings_screen.h"
#include "uart_pins_screen.h"
#include "vendor_lookup_screen.h"
#include "gps_module_screen.h"
#include "channel_time_settings_screen.h"
#include "settings.h"
#include "display.h"
#include "keyboard.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SETTINGS_SCREEN";

// Menu item indices
#define MENU_UART_PINS      0
#define MENU_VENDOR_LOOKUP  1
#define MENU_GPS_MODULE     2
#define MENU_CHANNEL_TIME   3
#define MENU_SCR_TIMEOUT    4
#define MENU_SCR_BRIGHT     5
#define MENU_SOUND          6
#define MENU_RED_TEAM       7
#define MENU_ITEM_COUNT     8
#define VISIBLE_ITEMS       6

// Screen dimming timeout options (in ms)
static const uint32_t timeout_options[] = { 10000, 30000, 60000, 300000, 0 };
static const char *timeout_labels[] = { "10s", "30s", "1min", "5min", "Stays On" };
#define TIMEOUT_OPTION_COUNT 5

// Screen user data
typedef struct {
    int selected_index;
    int scroll_offset;
    bool awaiting_disclaimer_confirm;  // Waiting for user to confirm disclaimer
} settings_screen_data_t;

/**
 * @brief Get the index of the current timeout value in timeout_options[]
 */
static int get_timeout_option_index(void)
{
    uint32_t current = settings_get_screen_timeout_ms();
    for (int i = 0; i < TIMEOUT_OPTION_COUNT; i++) {
        if (timeout_options[i] == current) {
            return i;
        }
    }
    return 1;  // Default to 30s if not found
}

/**
 * @brief Format a setting label with a right-aligned value
 * Builds a string like "Dimming       30s" that fills 30 columns
 */
static void format_setting_line(char *buf, size_t buf_size, const char *label, const char *value)
{
    int label_len = strlen(label);
    int value_len = strlen(value);
    int total_cols = 29;  // Leave 1 col margin
    int padding = total_cols - label_len - value_len;
    if (padding < 1) padding = 1;

    snprintf(buf, buf_size, "%s", label);
    int pos = label_len;
    for (int i = 0; i < padding && pos < (int)buf_size - 1; i++) {
        buf[pos++] = ' ';
    }
    snprintf(buf + pos, buf_size - pos, "%s", value);
}

static void draw_menu_item_at(int row, int index, bool selected)
{
    bool red_team = settings_get_red_team_enabled();
    bool sound_enabled = settings_get_sound_enabled();
    char line[40];

    switch (index) {
        case MENU_UART_PINS:
            ui_draw_menu_item(row, "UART Pins", selected, false, false);
            break;
        case MENU_VENDOR_LOOKUP:
            ui_draw_menu_item(row, "Vendor Lookup", selected, false, false);
            break;
        case MENU_GPS_MODULE:
            ui_draw_menu_item(row, "GPS", selected, false, false);
            break;
        case MENU_CHANNEL_TIME:
            ui_draw_menu_item(row, "Channel Time", selected, false, false);
            break;
        case MENU_SCR_TIMEOUT:
        {
            int idx = get_timeout_option_index();
            format_setting_line(line, sizeof(line), "Dimming", timeout_labels[idx]);
            ui_draw_menu_item(row, line, selected, false, false);
            break;
        }
        case MENU_SCR_BRIGHT:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", settings_get_screen_brightness());
            format_setting_line(line, sizeof(line), "Brightness", val);
            ui_draw_menu_item(row, line, selected, false, false);
            break;
        }
        case MENU_SOUND:
            ui_draw_menu_item(row, "Enable Sound", selected, true, sound_enabled);
            break;
        case MENU_RED_TEAM:
            ui_draw_menu_item(row, "Enable Red Team", selected, true, red_team);
            break;
    }
}

static int get_menu_row(const settings_screen_data_t *data, int index)
{
    return (index - data->scroll_offset) + 1;
}

static void draw_screen(screen_t *self)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;
    char status[32];
    uint8_t page_current = (uint8_t)((data->scroll_offset / VISIBLE_ITEMS) + 1);
    uint8_t page_total = (uint8_t)((MENU_ITEM_COUNT + VISIBLE_ITEMS - 1) / VISIBLE_ITEMS);

    ui_clear();

    // Draw title
    ui_draw_title("Settings");

    int visible_end = data->scroll_offset + VISIBLE_ITEMS;
    if (visible_end > MENU_ITEM_COUNT) {
        visible_end = MENU_ITEM_COUNT;
    }

    for (int i = data->scroll_offset; i < visible_end; i++) {
        draw_menu_item_at(get_menu_row(data, i), i, i == data->selected_index);
    }

    snprintf(status,
             sizeof(status),
             "UP/DN Nav ENT/Arw Pg %u/%u",
             (unsigned)page_current,
             (unsigned)page_total);
    ui_draw_status(status);
}

// Optimized: redraw only two changed rows
static void redraw_selection(settings_screen_data_t *data, int old_index, int new_index)
{
    if (old_index >= data->scroll_offset && old_index < data->scroll_offset + VISIBLE_ITEMS) {
        draw_menu_item_at(get_menu_row(data, old_index), old_index, false);
    }
    if (new_index >= data->scroll_offset && new_index < data->scroll_offset + VISIBLE_ITEMS) {
        draw_menu_item_at(get_menu_row(data, new_index), new_index, true);
    }
}

static void show_red_team_disclaimer(void)
{
    ui_show_message("DISCLAIMER",
        "Test YOUR networks only!");
}

/**
 * @brief Cycle screen timeout to the next/previous option
 * @param direction +1 for next, -1 for previous
 */
static void cycle_timeout(int direction)
{
    int idx = get_timeout_option_index();
    idx += direction;
    if (idx >= TIMEOUT_OPTION_COUNT) idx = 0;
    if (idx < 0) idx = TIMEOUT_OPTION_COUNT - 1;
    settings_set_screen_timeout_ms(timeout_options[idx]);
}

/**
 * @brief Adjust screen brightness
 * @param delta Amount to change (-100 to +100)
 */
static void adjust_brightness(int delta)
{
    int current = (int)settings_get_screen_brightness();
    current += delta;
    if (current < 1) current = 1;
    if (current > 100) current = 100;
    settings_set_screen_brightness((uint8_t)current);
    display_set_backlight((uint8_t)current);
}

static void on_key(screen_t *self, key_code_t key)
{
    settings_screen_data_t *data = (settings_screen_data_t *)self->user_data;

    // If awaiting disclaimer confirmation
    if (data->awaiting_disclaimer_confirm) {
        if (key == KEY_ENTER || key == KEY_SPACE) {
            // User accepted disclaimer - enable Red Team
            settings_set_red_team_enabled(true);
            data->awaiting_disclaimer_confirm = false;
            draw_screen(self);
        } else if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            // User declined - just redraw
            data->awaiting_disclaimer_confirm = false;
            draw_screen(self);
        }
        return;
    }

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= MENU_ITEM_COUNT) {
                        data->selected_index = MENU_ITEM_COUNT - 1;
                    }
                    draw_screen(self);
                } else {
                    data->selected_index--;
                    redraw_selection(data, old, data->selected_index);
                }
            } else {
                data->selected_index = MENU_ITEM_COUNT - 1;
                data->scroll_offset = (data->selected_index / VISIBLE_ITEMS) * VISIBLE_ITEMS;
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < MENU_ITEM_COUNT - 1) {
                int old = data->selected_index;
                data->selected_index++;
                if (data->selected_index >= data->scroll_offset + VISIBLE_ITEMS) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);
                } else {
                    redraw_selection(data, old, data->selected_index);
                }
            } else {
                data->selected_index = 0;
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_LEFT:
            if (data->selected_index == MENU_SCR_TIMEOUT) {
                cycle_timeout(-1);
                draw_menu_item_at(get_menu_row(data, MENU_SCR_TIMEOUT), MENU_SCR_TIMEOUT, true);
            } else if (data->selected_index == MENU_SCR_BRIGHT) {
                // Without Shift: -10%, with Shift: -1%
                int step = keyboard_is_shift_held() ? -1 : -10;
                adjust_brightness(step);
                draw_menu_item_at(get_menu_row(data, MENU_SCR_BRIGHT), MENU_SCR_BRIGHT, true);
            } else if (data->selected_index == MENU_SOUND && settings_get_sound_enabled()) {
                settings_set_sound_enabled(false);
                draw_menu_item_at(get_menu_row(data, MENU_SOUND), MENU_SOUND, true);
            }
            break;

        case KEY_RIGHT:
            if (data->selected_index == MENU_SCR_TIMEOUT) {
                cycle_timeout(+1);
                draw_menu_item_at(get_menu_row(data, MENU_SCR_TIMEOUT), MENU_SCR_TIMEOUT, true);
            } else if (data->selected_index == MENU_SCR_BRIGHT) {
                // Without Shift: +10%, with Shift: +1%
                int step = keyboard_is_shift_held() ? 1 : 10;
                adjust_brightness(step);
                draw_menu_item_at(get_menu_row(data, MENU_SCR_BRIGHT), MENU_SCR_BRIGHT, true);
            } else if (data->selected_index == MENU_SOUND && !settings_get_sound_enabled()) {
                settings_set_sound_enabled(true);
                draw_menu_item_at(get_menu_row(data, MENU_SOUND), MENU_SOUND, true);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
        {
            switch (data->selected_index) {
                case MENU_UART_PINS:
                    screen_manager_push(uart_pins_screen_create, NULL);
                    break;
                case MENU_VENDOR_LOOKUP:
                    screen_manager_push(vendor_lookup_screen_create, NULL);
                    break;
                case MENU_GPS_MODULE:
                    screen_manager_push(gps_module_screen_create, NULL);
                    break;
                case MENU_CHANNEL_TIME:
                    screen_manager_push(channel_time_settings_screen_create, NULL);
                    break;
                case MENU_SCR_TIMEOUT:
                    // ENTER also cycles timeout forward
                    cycle_timeout(+1);
                    draw_menu_item_at(get_menu_row(data, MENU_SCR_TIMEOUT), MENU_SCR_TIMEOUT, true);
                    break;
                case MENU_SCR_BRIGHT:
                    // No action on ENTER for brightness (use arrows)
                    break;
                case MENU_SOUND:
                    settings_set_sound_enabled(!settings_get_sound_enabled());
                    draw_menu_item_at(get_menu_row(data, MENU_SOUND), MENU_SOUND, true);
                    break;
                case MENU_RED_TEAM:
                    if (settings_get_red_team_enabled()) {
                        // Already enabled - just disable it
                        settings_set_red_team_enabled(false);
                        draw_menu_item_at(get_menu_row(data, MENU_RED_TEAM), MENU_RED_TEAM, true);
                    } else {
                        // Show disclaimer before enabling
                        show_red_team_disclaimer();
                        data->awaiting_disclaimer_confirm = true;
                    }
                    break;
            }
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
    draw_screen(self);
}

screen_t* settings_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating settings screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    // Allocate user data
    settings_screen_data_t *data = calloc(1, sizeof(settings_screen_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    // Draw initial screen
    draw_screen(screen);

    ESP_LOGI(TAG, "Settings screen created");
    return screen;
}
