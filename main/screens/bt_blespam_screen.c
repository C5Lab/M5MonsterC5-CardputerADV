/**
 * @file bt_blespam_screen.c
 * @brief IOS/Samsung BLE spam screen
 *
 * Starts JanOS `start_blespam` on entry and shows "Spam Active".
 * Esc / Q / Backspace send `stop` and return to the Bluetooth menu.
 */

#include "bt_blespam_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "BT_BLESPAM";

typedef struct {
    bool active;
} bt_blespam_data_t;

static void draw_screen(screen_t *self)
{
    (void)self;

    ui_clear();
    ui_draw_title("IOS/Samsung Spam");
    ui_print_center(3, "Spam Active", UI_COLOR_HIGHLIGHT);
    ui_draw_status("ESC: Stop & Exit");
}

static void stop_spam(bt_blespam_data_t *data)
{
    if (!data || !data->active) {
        return;
    }
    uart_send_command("stop");
    data->active = false;
}

static void on_key(screen_t *self, key_code_t key)
{
    bt_blespam_data_t *data = (bt_blespam_data_t *)self->user_data;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            stop_spam(data);
            screen_manager_pop();
            break;

        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    bt_blespam_data_t *data = (bt_blespam_data_t *)self->user_data;

    stop_spam(data);
    uart_clear_line_callback();

    if (data) {
        free(data);
        self->user_data = NULL;
    }
}

screen_t* bt_blespam_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating BLE spam screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    bt_blespam_data_t *data = calloc(1, sizeof(bt_blespam_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->active = true;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;

    uart_send_command("start_blespam");
    buzzer_beep_attack();

    draw_screen(screen);

    ESP_LOGI(TAG, "BLE spam screen created, spam started");
    return screen;
}
