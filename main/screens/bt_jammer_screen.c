/**
 * @file bt_jammer_screen.c
 * @brief Bluetooth nRF24 jammer screen
 *
 * Mirrors the Tab5 BT jammer UART sequence, adapted to the Cardputer's single
 * UART and keyboard text UI:
 *   start:  init_nrf24   then  start_jammer24 <band>
 *   stop:   stop
 * Status is driven by JanOS lines: "[NRF24] not detected",
 * "[NRF24] failed to start", "nRF24 jammer started".
 */

#include "bt_jammer_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "BT_JAM";

static const char *k_jam_bands[5] = { "ble", "bt", "wifi", "drone", "all" };
#define BAND_COUNT 5

typedef struct {
    int  band_index;
    bool jamming;
    char status[40];
    bool needs_redraw;
} bt_jammer_data_t;

static void draw_screen(screen_t *self)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("BT Jammer");

    char band_buf[32];
    snprintf(band_buf, sizeof(band_buf), "Band: %s", k_jam_bands[data->band_index]);
    ui_print_center(2, band_buf, UI_COLOR_HIGHLIGHT);

    if (data->jamming) {
        ui_print_center(4, ">> JAMMING <<", UI_COLOR_TITLE);
    } else {
        ui_print_center(4, "Idle", UI_COLOR_DIMMED);
        ui_print_center(6, "UP/DOWN: Band", UI_COLOR_DIMMED);
    }

    if (data->status[0] != '\0') {
        ui_print_center(5, data->status, UI_COLOR_TEXT);
    } else {
        ui_print_center(5, data->jamming ? "[ENTER] Stop" : "[ENTER] Start", UI_COLOR_TEXT);
    }

    ui_draw_status("ENTER:Start/Stop UP/DN:Band ESC:Back");
}

static void start_jamming(screen_t *self)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;
    if (data->jamming) return;

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "start_jammer24 %s", k_jam_bands[data->band_index]);
    uart_send_command("init_nrf24");
    uart_send_command(cmd);

    data->jamming = true;
    snprintf(data->status, sizeof(data->status), "Starting %s...", k_jam_bands[data->band_index]);
    ESP_LOGI(TAG, "Jammer start: %s", k_jam_bands[data->band_index]);
    draw_screen(self);
}

static void stop_jamming(screen_t *self)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;
    if (!data->jamming) return;

    uart_send_command("stop");
    data->jamming = false;
    data->status[0] = '\0';
    ESP_LOGI(TAG, "Jammer stopped");
    draw_screen(self);
}

static void uart_line_callback(const char *line, void *user_data)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)user_data;
    if (!data || !line) return;

    if (strstr(line, "[NRF24] not detected")) {
        strncpy(data->status, "Module not detected!", sizeof(data->status) - 1);
        data->status[sizeof(data->status) - 1] = '\0';
        data->needs_redraw = true;
    } else if (strstr(line, "[NRF24] failed to start")) {
        strncpy(data->status, "Failed to start", sizeof(data->status) - 1);
        data->status[sizeof(data->status) - 1] = '\0';
        data->needs_redraw = true;
    } else if (strstr(line, "nRF24 jammer started")) {
        snprintf(data->status, sizeof(data->status), "Jamming %s...", k_jam_bands[data->band_index]);
        data->needs_redraw = true;
    }
}

static void on_tick(screen_t *self)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;

    switch (key) {
        case KEY_UP:
        case KEY_LEFT:
            if (!data->jamming) {
                data->band_index = (data->band_index + BAND_COUNT - 1) % BAND_COUNT;
                draw_screen(self);
            }
            break;

        case KEY_DOWN:
        case KEY_RIGHT:
            if (!data->jamming) {
                data->band_index = (data->band_index + 1) % BAND_COUNT;
                draw_screen(self);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->jamming) stop_jamming(self);
            else               start_jamming(self);
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->jamming) {
                uart_send_command("stop");
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
    bt_jammer_data_t *data = (bt_jammer_data_t *)self->user_data;

    if (data && data->jamming) {
        uart_send_command("stop");
        data->jamming = false;
    }
    uart_clear_line_callback();

    if (data) {
        free(data);
        self->user_data = NULL;
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

screen_t* bt_jammer_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating BT jammer screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    bt_jammer_data_t *data = calloc(1, sizeof(bt_jammer_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->band_index = 0; // ble
    data->jamming = false;
    data->status[0] = '\0';

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(uart_line_callback, data);

    draw_screen(screen);

    ESP_LOGI(TAG, "BT jammer screen created");
    return screen;
}
