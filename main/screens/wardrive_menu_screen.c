/**
 * @file wardrive_menu_screen.c
 * @brief Wardrive sub-menu (scrollable; content-only redraw on nav).
 */

#include "wardrive_menu_screen.h"
#include "wardrive_screen.h"
#include "wardrive_upload_screen.h"
#include "wardrive_config_screen.h"
#include "wardrive_blacklist_screen.h"
#include "antisurv_screen.h"
#include "text_ui.h"
#include <stdlib.h>
#include <stdio.h>

// Menu item indices
#define MI_START     0
#define MI_ANTISURV  1
#define MI_SETUP     2
#define MI_BLACKLIST 3
#define MI_WDGWARS   4
#define MI_WIGLE     5
#define MI_TRACE     6
#define MI_COUNT     7

#define VISIBLE_ITEMS 6

typedef struct {
    int selected;
    int scroll_offset;
    bool trace_enabled;
} wardrive_menu_data_t;

// Build the label for one item (trace is dynamic).
static void item_label(wardrive_menu_data_t *data, int idx, char *buf, size_t len)
{
    switch (idx) {
        case MI_START:     snprintf(buf, len, "Start Wardrive"); break;
        case MI_ANTISURV:  snprintf(buf, len, "Anti-Surveillance"); break;
        case MI_SETUP:     snprintf(buf, len, "Setup"); break;
        case MI_BLACKLIST: snprintf(buf, len, "Blacklist"); break;
        case MI_WDGWARS:   snprintf(buf, len, "Upload to Wdgwars"); break;
        case MI_WIGLE:     snprintf(buf, len, "Upload to Wigle"); break;
        case MI_TRACE:     snprintf(buf, len, "Trace: %s", data->trace_enabled ? "Yes" : "No"); break;
        default:           buf[0] = '\0'; break;
    }
}

// Draw the visible menu items + scroll arrows (no clear/title/status).
static void redraw_items(wardrive_menu_data_t *data)
{
    for (int r = 0; r < VISIBLE_ITEMS; r++) {
        int idx = data->scroll_offset + r;
        int row = r + 1;
        if (idx < MI_COUNT) {
            char label[24];
            item_label(data, idx, label, sizeof(label));
            ui_draw_menu_item(row, label, idx == data->selected, false, false);
        } else {
            display_fill_rect(0, row * 16, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < MI_COUNT) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static void draw_screen(screen_t *self)
{
    wardrive_menu_data_t *data = (wardrive_menu_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Wardrive");
    redraw_items(data);
    ui_draw_status("UP/DOWN OK BACK RIGHT");
}

static void move_selection(wardrive_menu_data_t *data, int dir)
{
    int s = data->selected + dir;
    if (s < 0) s = MI_COUNT - 1;
    if (s >= MI_COUNT) s = 0;
    data->selected = s;
    if (data->selected < data->scroll_offset) data->scroll_offset = data->selected;
    if (data->selected >= data->scroll_offset + VISIBLE_ITEMS)
        data->scroll_offset = data->selected - VISIBLE_ITEMS + 1;
}

static void start_wardrive_run(wardrive_menu_data_t *data)
{
    wardrive_run_params_t *p = malloc(sizeof(wardrive_run_params_t));
    if (!p) return;
    p->trace = data->trace_enabled;
    screen_manager_push(wardrive_screen_create, p);
}

static void start_upload(wardrive_upload_target_t target)
{
    wardrive_upload_params_t *p = malloc(sizeof(wardrive_upload_params_t));
    if (!p) return;
    p->target = target;
    screen_manager_push(wardrive_upload_screen_create, p);
}

static void on_key(screen_t *self, key_code_t key)
{
    wardrive_menu_data_t *data = (wardrive_menu_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_UP:
            move_selection(data, -1);
            redraw_items(data);
            break;
        case KEY_DOWN:
            move_selection(data, +1);
            redraw_items(data);
            break;
        case KEY_RIGHT:
            if (data->selected == MI_TRACE) {
                data->trace_enabled = !data->trace_enabled;
                redraw_items(data);
            }
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            switch (data->selected) {
                case MI_START:     start_wardrive_run(data); break;
                case MI_ANTISURV:  screen_manager_push(antisurv_screen_create, NULL); break;
                case MI_SETUP:     screen_manager_push(wardrive_config_screen_create, NULL); break;
                case MI_BLACKLIST: screen_manager_push(wardrive_blacklist_screen_create, NULL); break;
                case MI_WDGWARS:   start_upload(WARDRIVE_UPLOAD_TARGET_WDGWARS); break;
                case MI_WIGLE:     start_upload(WARDRIVE_UPLOAD_TARGET_WIGLE); break;
                case MI_TRACE:
                    data->trace_enabled = !data->trace_enabled;
                    redraw_items(data);
                    break;
                default: break;
            }
            break;
        case KEY_ESC:
        case KEY_BACKSPACE:
        case KEY_Q:
            screen_manager_pop();
            break;
        default:
            break;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    if (self->user_data) free(self->user_data);
}

screen_t* wardrive_menu_screen_create(void *params)
{
    (void)params;
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    wardrive_menu_data_t *data = calloc(1, sizeof(wardrive_menu_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    data->selected = 0;
    data->scroll_offset = 0;
    data->trace_enabled = true;

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_key = on_key;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    draw_screen(screen);
    return screen;
}
