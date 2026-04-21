#include "beacon_spam_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BEACON_SPAM";

typedef struct {
    int ssid_count;
    bool needs_redraw;
} beacon_spam_data_t;

static void draw_screen(screen_t *self)
{
    beacon_spam_data_t *data = (beacon_spam_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Beacon Spam");

    ui_print_center(3, "Beacon spam active", UI_COLOR_HIGHLIGHT);

    if (data->ssid_count > 0) {
        char info[32];
        snprintf(info, sizeof(info), "%d SSIDs broadcasting", data->ssid_count);
        ui_print_center(5, info, UI_COLOR_TEXT);
    }

    ui_draw_status("ESC: Stop & Exit");
}

static void uart_line_callback(const char *line, void *user_data)
{
    beacon_spam_data_t *data = (beacon_spam_data_t *)user_data;
    if (!data) return;

    // "Starting beacon spam with N SSIDs:"
    const char *match = strstr(line, "Starting beacon spam with ");
    if (match) {
        int n = atoi(match + 26);
        if (n > 0) {
            data->ssid_count = n;
            data->needs_redraw = true;
        }
    }
}

static void on_tick(screen_t *self)
{
    beacon_spam_data_t *data = (beacon_spam_data_t *)self->user_data;
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    (void)self;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            uart_send_command("stop");
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

screen_t* beacon_spam_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating beacon spam screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    beacon_spam_data_t *data = calloc(1, sizeof(beacon_spam_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("start_beacon_spam_ssids");
    buzzer_beep_attack();

    draw_screen(screen);

    ESP_LOGI(TAG, "Beacon spam screen created, attack started");
    return screen;
}
