/**
 * @file mitm_sniffer_screen.c
 * @brief MITM network sniffer screen implementation
 *
 * Sends start_pcap net on create, shows sniffing status.
 * On ESC sends stop and pops back.
 */

#include "mitm_sniffer_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "MITM_SNIFF";

static void draw_screen(screen_t *self)
{
    (void)self;

    ui_clear();
    ui_draw_title("MITM Sniffer");
    ui_print_center(3, "Sniffing...", UI_COLOR_HIGHLIGHT);
    ui_draw_status("ESC: Stop & Exit");
}

static void on_key(screen_t *self, key_code_t key)
{
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
    (void)self;
}

screen_t* mitm_sniffer_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating MITM sniffer screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    screen->user_data = NULL;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;

    uart_send_command("start_pcap net");

    draw_screen(screen);

    ESP_LOGI(TAG, "MITM sniffer screen created, capture started");
    return screen;
}
