/**
 * @file subghz_jammer_screen.c
 * @brief Sub-GHz jammer screen
 *
 * UART sequence mirrors coreS3/main/screens/subghz_jammer_screen.c exactly:
 *   start:  subghz_freq <f>   then  subghz_jam
 *   stop:   subghz_stop
 */

#include "subghz_jammer_screen.h"
#include "subghz_freq_picker_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SUBGHZ_JAM";

typedef struct {
    float freq_mhz;
    bool  jamming;
} subghz_jammer_data_t;

/* The Cardputer keeps only one jammer screen alive at a time, but the
 * frequency picker callback fires while we are not the top screen anymore,
 * so we need a stable reference to the data block. */
static subghz_jammer_data_t *s_current_data = NULL;

static void draw_screen(screen_t *self)
{
    subghz_jammer_data_t *data = (subghz_jammer_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Jammer");

    char freq_buf[32];
    int whole = (int)data->freq_mhz;
    int frac  = ((int)(data->freq_mhz * 100.0f + 0.5f)) % 100;
    snprintf(freq_buf, sizeof(freq_buf), "Freq: %d.%02d MHz", whole, frac);
    ui_print_center(2, freq_buf, UI_COLOR_HIGHLIGHT);

    if (data->jamming) {
        ui_print_center(4, ">> JAMMING <<", UI_COLOR_TITLE);
        ui_print_center(5, "[ENTER/SPC] Stop", UI_COLOR_TEXT);
    } else {
        ui_print_center(4, "Idle", UI_COLOR_DIMMED);
        ui_print_center(5, "[ENTER/SPC] Start", UI_COLOR_TEXT);
    }

    ui_print_center(6, "[F] Change freq", UI_COLOR_DIMMED);

    ui_draw_status("ENTER:Start/Stop F:Freq ESC:Back");
}

static void start_jamming(screen_t *self)
{
    subghz_jammer_data_t *data = (subghz_jammer_data_t *)self->user_data;
    if (data->jamming) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", data->freq_mhz);
    uart_send_command(cmd);
    uart_send_command("subghz_jam");

    data->jamming = true;
    ESP_LOGI(TAG, "Jammer started on %.2f MHz", data->freq_mhz);
    draw_screen(self);
}

static void stop_jamming(screen_t *self)
{
    subghz_jammer_data_t *data = (subghz_jammer_data_t *)self->user_data;
    if (!data->jamming) return;

    uart_send_command("subghz_stop");
    data->jamming = false;
    ESP_LOGI(TAG, "Jammer stopped");
    draw_screen(self);
}

static void on_freq_picked(float freq, void *user_data)
{
    (void)user_data;
    if (!s_current_data) return;

    if (s_current_data->jamming) {
        /* Switching frequency mid-stream: stop and restart on the new freq. */
        uart_send_command("subghz_stop");
        s_current_data->jamming = false;
    }
    s_current_data->freq_mhz = freq;
    ESP_LOGI(TAG, "Jammer freq updated to %.2f MHz", freq);
    /* draw_screen will be called via on_resume after the freq picker pops. */
}

static void on_key(screen_t *self, key_code_t key)
{
    subghz_jammer_data_t *data = (subghz_jammer_data_t *)self->user_data;

    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->jamming) stop_jamming(self);
            else               start_jamming(self);
            break;

        case KEY_F:
            if (!data->jamming) {
                subghz_freq_picker_params_t *p = calloc(1, sizeof(subghz_freq_picker_params_t));
                if (p) {
                    p->initial_freq = data->freq_mhz;
                    p->on_pick = on_freq_picked;
                    p->user_data = NULL;
                    screen_manager_push(subghz_freq_picker_screen_create, p);
                }
            }
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->jamming) {
                uart_send_command("subghz_stop");
                data->jamming = false;
            }
            screen_manager_pop();
            break;

        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    subghz_jammer_data_t *data = (subghz_jammer_data_t *)self->user_data;
    if (data) {
        if (data->jamming) {
            uart_send_command("subghz_stop");
            data->jamming = false;
        }
        if (s_current_data == data) s_current_data = NULL;
        free(data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* subghz_jammer_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_jammer_data_t *data = calloc(1, sizeof(subghz_jammer_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->freq_mhz = 433.92f;
    data->jamming  = false;
    s_current_data = data;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;

    draw_screen(screen);
    ESP_LOGI(TAG, "Jammer screen created");
    return screen;
}
