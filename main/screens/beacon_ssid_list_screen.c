#include "beacon_ssid_list_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "BEACON_SSID_LIST";

#define MAX_ENTRIES     64
#define MAX_SSID_LEN    33
#define VISIBLE_ITEMS   6

typedef struct {
    char ssids[MAX_ENTRIES][MAX_SSID_LEN];
    int entry_count;
    int selected_index;
    int scroll_offset;
    bool loading;
    bool needs_redraw;
    bool first_draw_done;
    int ticks_since_first_draw;
} beacon_ssid_list_data_t;

static void draw_screen(screen_t *self);

static bool is_skip_line(const char *line)
{
    if (strlen(line) < 3) return true;

    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D')
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }

    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "list_ssids", 10) == 0) return true;

    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return true;

    return false;
}

static void uart_line_callback(const char *line, void *user_data)
{
    beacon_ssid_list_data_t *data = (beacon_ssid_list_data_t *)user_data;
    if (!data || data->entry_count >= MAX_ENTRIES) return;
    if (strlen(line) == 0) return;
    if (is_skip_line(line)) return;

    if (strstr(line, "not found") != NULL || strstr(line, "empty") != NULL ||
        strstr(line, "Empty") != NULL) {
        data->loading = false;
        if (data->first_draw_done) {
            data->needs_redraw = true;
        }
        return;
    }

    // Parse "N SSID_TEXT" where N is a number followed by a space
    const char *p = line;
    while (*p && isdigit((unsigned char)*p)) p++;
    if (p == line || *p != ' ') return;
    p++; // skip space

    if (*p == '\0') return;

    strncpy(data->ssids[data->entry_count], p, MAX_SSID_LEN - 1);
    data->ssids[data->entry_count][MAX_SSID_LEN - 1] = '\0';
    data->entry_count++;
    data->loading = false;

    if (data->first_draw_done) {
        data->needs_redraw = true;
    }
}

static void draw_screen(screen_t *self)
{
    beacon_ssid_list_data_t *data = (beacon_ssid_list_data_t *)self->user_data;

    ui_clear();

    char title[32];
    snprintf(title, sizeof(title), "SSIDs (%d)", data->entry_count);
    ui_draw_title(title);

    if (data->loading) {
        ui_print_center(3, "Loading...", UI_COLOR_DIMMED);
    } else if (data->entry_count == 0) {
        ui_print_center(3, "No SSIDs found", UI_COLOR_DIMMED);
    } else {
        int start_row = 1;

        for (int i = 0; i < VISIBLE_ITEMS; i++) {
            int entry_idx = data->scroll_offset + i;

            if (entry_idx < data->entry_count) {
                char label[40];
                snprintf(label, sizeof(label), "%d.%.26s", entry_idx + 1, data->ssids[entry_idx]);
                label[29] = '\0';

                bool selected = (entry_idx == data->selected_index);
                ui_draw_menu_item(start_row + i, label, selected, false, false);
            }
        }

        if (data->scroll_offset > 0) {
            ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
        }
        if (data->scroll_offset + VISIBLE_ITEMS < data->entry_count) {
            ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
        }
    }

    ui_draw_status("A-Add D-Del ESC:Back");
}

static void reload_list(beacon_ssid_list_data_t *data)
{
    data->entry_count = 0;
    data->selected_index = 0;
    data->scroll_offset = 0;
    data->loading = true;
    data->needs_redraw = false;
    data->first_draw_done = false;
    data->ticks_since_first_draw = 0;
    uart_send_command("list_ssids");
}

static void on_ssid_added(const char *text, void *user_data)
{
    (void)user_data;

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "add_ssid \"%s\"", text);
    uart_send_command(cmd);

    screen_manager_pop();
}

static void on_tick(screen_t *self)
{
    beacon_ssid_list_data_t *data = (beacon_ssid_list_data_t *)self->user_data;

    if (!data->first_draw_done) {
        data->first_draw_done = true;
        data->ticks_since_first_draw = 0;
        data->needs_redraw = false;
        draw_screen(self);
        return;
    }

    data->ticks_since_first_draw++;

    if (data->ticks_since_first_draw <= 2) {
        return;
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    beacon_ssid_list_data_t *data = (beacon_ssid_list_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= data->entry_count) {
                        data->selected_index = data->entry_count - 1;
                    }
                    draw_screen(self);
                } else {
                    data->selected_index--;
                    int start_row = 1;
                    for (int idx = old_idx; idx >= data->selected_index; idx--) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < VISIBLE_ITEMS && idx < data->entry_count) {
                            char label[40];
                            snprintf(label, sizeof(label), "%d.%.26s", idx + 1, data->ssids[idx]);
                            label[29] = '\0';
                            ui_draw_menu_item(start_row + i, label, idx == data->selected_index, false, false);
                        }
                    }
                }
            } else if (data->entry_count > 0) {
                data->selected_index = data->entry_count - 1;
                data->scroll_offset = (data->selected_index / VISIBLE_ITEMS) * VISIBLE_ITEMS;
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < data->entry_count - 1) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    draw_screen(self);
                } else {
                    data->selected_index++;
                    int start_row = 1;
                    for (int idx = old_idx; idx <= data->selected_index; idx++) {
                        int i = idx - data->scroll_offset;
                        if (i >= 0 && i < VISIBLE_ITEMS && idx < data->entry_count) {
                            char label[40];
                            snprintf(label, sizeof(label), "%d.%.26s", idx + 1, data->ssids[idx]);
                            label[29] = '\0';
                            ui_draw_menu_item(start_row + i, label, idx == data->selected_index, false, false);
                        }
                    }
                }
            } else if (data->entry_count > 0) {
                data->selected_index = 0;
                data->scroll_offset = 0;
                draw_screen(self);
            }
            break;

        case KEY_A:
            {
                text_input_params_t *params = malloc(sizeof(text_input_params_t));
                if (params) {
                    params->title = "Add SSID";
                    params->hint = "Type SSID, ENTER to add";
                    params->on_submit = on_ssid_added;
                    params->user_data = NULL;
                    screen_manager_push(text_input_screen_create, params);
                }
            }
            break;

        case KEY_D:
            if (data->entry_count > 0 && data->selected_index < data->entry_count) {
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "remove_ssid %d", data->selected_index + 1);
                uart_send_command(cmd);

                uart_register_line_callback(uart_line_callback, data);
                reload_list(data);
                draw_screen(self);
            }
            break;

        case KEY_ESC:
        case KEY_BACKSPACE:
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
    beacon_ssid_list_data_t *data = (beacon_ssid_list_data_t *)self->user_data;
    uart_register_line_callback(uart_line_callback, data);
    reload_list(data);
    draw_screen(self);
}

screen_t* beacon_ssid_list_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating beacon SSID list screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    beacon_ssid_list_data_t *data = calloc(1, sizeof(beacon_ssid_list_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->loading = true;
    data->first_draw_done = false;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("list_ssids");

    ESP_LOGI(TAG, "Beacon SSID list screen created");
    return screen;
}
