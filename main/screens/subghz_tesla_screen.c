/**
 * @file subghz_tesla_screen.c
 * @brief Tesla charge-port opener (single-shot subghz_freq 315.00 + subghz_tx tesla)
 *
 * Mirrors coreS3/main/screens/subghz_tesla_screen.c exactly in UART behaviour.
 */

#include "subghz_tesla_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SUBGHZ_TESLA";

typedef struct {
    int  send_count;
    bool last_sent;
} subghz_tesla_data_t;

static void draw_screen(screen_t *self)
{
    subghz_tesla_data_t *data = (subghz_tesla_data_t *)self->user_data;

    ui_clear();
    ui_draw_title("Tesla Port");

    ui_print_center(2, "Charge Port Opener", UI_COLOR_HIGHLIGHT);
    ui_print_center(3, "315.00 MHz OOK", UI_COLOR_DIMMED);

    ui_print_center(5, "[ENTER] Open Port", UI_COLOR_TEXT);

    if (data->last_sent) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Sent! (%d)", data->send_count);
        ui_print_center(6, buf, UI_COLOR_HIGHLIGHT);
    } else {
        ui_print_center(6, "Ready", UI_COLOR_DIMMED);
    }

    ui_draw_status("ENTER:Send ESC:Back");
}

static void send_tesla(screen_t *self)
{
    subghz_tesla_data_t *data = (subghz_tesla_data_t *)self->user_data;
    /* Identical sequence to coreS3 subghz_tesla_screen.c */
    uart_send_command("subghz_freq 315.00");
    uart_send_command("subghz_tx tesla");
    data->send_count++;
    data->last_sent = true;
    ESP_LOGI(TAG, "Tesla charge port signal sent (315 MHz), count=%d", data->send_count);
    draw_screen(self);
}

static void on_key(screen_t *self, key_code_t key)
{
    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            send_tesla(self);
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
        self->user_data = NULL;
    }
}

screen_t* subghz_tesla_screen_create(void *params)
{
    (void)params;

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    subghz_tesla_data_t *data = calloc(1, sizeof(subghz_tesla_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;

    draw_screen(screen);
    ESP_LOGI(TAG, "Tesla screen created");
    return screen;
}
