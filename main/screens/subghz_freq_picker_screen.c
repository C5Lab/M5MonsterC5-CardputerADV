/**
 * @file subghz_freq_picker_screen.c
 * @brief Frequency picker for SubGHz screens (presets + custom text input)
 */

#include "subghz_freq_picker_screen.h"
#include "text_input_screen.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SUBGHZ_FREQ";

#define ITEM_COUNT 5

static const char *s_item_labels[ITEM_COUNT] = {
    "315 MHz",
    "433.92 MHz",
    "868 MHz",
    "915 MHz",
    "Custom...",
};

static const float s_item_freqs[ITEM_COUNT] = {
    315.00f,
    433.92f,
    868.00f,
    915.00f,
    0.0f,  /* Custom */
};

typedef struct {
    int selected_index;
    float initial_freq;
    subghz_freq_picker_cb_t on_pick;
    void *user_data;
} subghz_freq_picker_data_t;

static void draw_screen(screen_t *self)
{
    subghz_freq_picker_data_t *data = (subghz_freq_picker_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Sub-GHz Frequency");

    for (int i = 0; i < ITEM_COUNT; i++) {
        ui_draw_menu_item(i + 1, s_item_labels[i],
                          data->selected_index == i, false, false);
    }

    char hint[32];
    int whole = (int)data->initial_freq;
    int frac  = ((int)(data->initial_freq * 100.0f + 0.5f)) % 100;
    snprintf(hint, sizeof(hint), "Current: %d.%02d MHz", whole, frac);
    ui_print(0, 6, hint, UI_COLOR_DIMMED);

    ui_draw_status("UP/DOWN ENTER:Pick ESC:Cancel");
}

static void redraw_two(subghz_freq_picker_data_t *data, int old_idx, int new_idx)
{
    if (old_idx >= 0 && old_idx < ITEM_COUNT) {
        ui_draw_menu_item(old_idx + 1, s_item_labels[old_idx], false, false, false);
    }
    if (new_idx >= 0 && new_idx < ITEM_COUNT) {
        ui_draw_menu_item(new_idx + 1, s_item_labels[new_idx], true, false, false);
    }
}

static void on_custom_freq_submitted(const char *text, void *user_data)
{
    subghz_freq_picker_data_t *data = (subghz_freq_picker_data_t *)user_data;
    if (!data) return;

    float freq = (float)atof(text);
    subghz_freq_picker_cb_t cb = data->on_pick;
    void *cb_user = data->user_data;

    /* Pop text input then the freq picker. data is freed in our on_destroy. */
    screen_manager_pop();
    screen_manager_pop();

    if (cb && freq > 0.0f && freq <= 9999.99f) {
        ESP_LOGI(TAG, "Custom freq selected: %.2f MHz", freq);
        cb(freq, cb_user);
    } else {
        ESP_LOGW(TAG, "Invalid custom freq '%s' (parsed %.2f)", text, freq);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_freq_picker_data_t *data = (subghz_freq_picker_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
            if (data->selected_index > 0) {
                int old = data->selected_index;
                data->selected_index--;
                redraw_two(data, old, data->selected_index);
            } else {
                int old = data->selected_index;
                data->selected_index = ITEM_COUNT - 1;
                redraw_two(data, old, data->selected_index);
            }
            break;

        case KEY_DOWN:
            if (data->selected_index < ITEM_COUNT - 1) {
                int old = data->selected_index;
                data->selected_index++;
                redraw_two(data, old, data->selected_index);
            } else {
                int old = data->selected_index;
                data->selected_index = 0;
                redraw_two(data, old, data->selected_index);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE: {
            int idx = data->selected_index;
            if (idx >= 0 && idx < ITEM_COUNT - 1) {
                float freq = s_item_freqs[idx];
                subghz_freq_picker_cb_t cb = data->on_pick;
                void *cb_user = data->user_data;
                screen_manager_pop();
                if (cb) cb(freq, cb_user);
            } else if (idx == ITEM_COUNT - 1) {
                /* Custom... -> text input */
                text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
                if (p) {
                    p->title = "Frequency (MHz)";
                    p->hint = "e.g. 433.92";
                    p->on_submit = on_custom_freq_submitted;
                    p->user_data = data;
                    p->allow_empty = false;
                    screen_manager_push(text_input_screen_create, p);
                }
            }
            break;
        }

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
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_freq_picker_screen_create(void *params)
{
    subghz_freq_picker_params_t *p = (subghz_freq_picker_params_t *)params;
    if (!p) return NULL;

    screen_t *screen = screen_alloc();
    if (!screen) {
        free(p);
        return NULL;
    }

    subghz_freq_picker_data_t *data = calloc(1, sizeof(subghz_freq_picker_data_t));
    if (!data) {
        free(screen);
        free(p);
        return NULL;
    }

    data->initial_freq = p->initial_freq;
    data->on_pick = p->on_pick;
    data->user_data = p->user_data;
    data->selected_index = 1; /* default highlight 433.92 */

    free(p);

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);
    return screen;
}
