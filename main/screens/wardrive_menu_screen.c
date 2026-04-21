/**
 * @file wardrive_menu_screen.c
 * @brief Wardrive sub-menu
 */

#include "wardrive_menu_screen.h"
#include "wardrive_screen.h"
#include "wardrive_upload_screen.h"
#include "text_ui.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    int selected;
    bool trace_enabled;
} wardrive_menu_data_t;

static void draw_screen(screen_t *self)
{
    wardrive_menu_data_t *data = (wardrive_menu_data_t *)self->user_data;
    ui_clear();
    ui_draw_title("Wardrive");

    char trace[24];
    snprintf(trace, sizeof(trace), "Trace: %s", data->trace_enabled ? "Yes" : "No");

    ui_draw_menu_item(1, "Start Wardrive", data->selected == 0, false, false);
    ui_draw_menu_item(2, "Upload to Wdgwars", data->selected == 1, false, false);
    ui_draw_menu_item(3, "Upload to Wigle", data->selected == 2, false, false);
    ui_draw_menu_item(4, trace, data->selected == 3, false, false);
    ui_draw_status("UP/DOWN OK BACK RIGHT");
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
            data->selected = (data->selected > 0) ? data->selected - 1 : 3;
            draw_screen(self);
            break;
        case KEY_DOWN:
            data->selected = (data->selected < 3) ? data->selected + 1 : 0;
            draw_screen(self);
            break;
        case KEY_RIGHT:
            if (data->selected == 3) {
                data->trace_enabled = !data->trace_enabled;
                draw_screen(self);
            }
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->selected == 0) {
                start_wardrive_run(data);
            } else if (data->selected == 1) {
                start_upload(WARDRIVE_UPLOAD_TARGET_WDGWARS);
            } else if (data->selected == 2) {
                start_upload(WARDRIVE_UPLOAD_TARGET_WIGLE);
            } else if (data->selected == 3) {
                data->trace_enabled = !data->trace_enabled;
                draw_screen(self);
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
    data->trace_enabled = true;

    screen->user_data = data;
    screen->on_draw = draw_screen;
    screen->on_key = on_key;
    screen->on_resume = on_resume;
    screen->on_destroy = on_destroy;

    draw_screen(screen);
    return screen;
}
