/**
 * @file radio_sniffer_screen.c
 * @brief Radio PCAP capture screen
 *
 * Sends start_pcap radio and shows the UART start line verbatim.
 * ESC sends stop and holds the "PCAP saved:" line until a second ESC.
 */

#include "radio_sniffer_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "RADIO_SNIFF";

enum {
    PHASE_STARTING = 0,
    PHASE_CAPTURING,
    PHASE_STOPPING,
    PHASE_SAVED,
    PHASE_ERROR,
};

#define LINE_MAX 192

typedef struct {
    int phase;
    bool needs_redraw;
    bool stop_sent;
    char line[LINE_MAX];
} radio_sniffer_data_t;

static void copy_line(radio_sniffer_data_t *data, const char *line)
{
    strncpy(data->line, line, sizeof(data->line) - 1);
    data->line[sizeof(data->line) - 1] = '\0';
}

static void uart_line_callback(const char *line, void *user_data)
{
    radio_sniffer_data_t *data = (radio_sniffer_data_t *)user_data;
    if (!data || !line) return;

    int phase = __atomic_load_n(&data->phase, __ATOMIC_ACQUIRE);

    if (phase == PHASE_STARTING && strstr(line, "PCAP radio capture started")) {
        copy_line(data, line);
        int expected = PHASE_STARTING;
        if (__atomic_compare_exchange_n(&data->phase, &expected, PHASE_CAPTURING,
                                        false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        }
        return;
    }

    if (phase == PHASE_STARTING &&
        (strstr(line, "Failed to") || strstr(line, "Usage: start_pcap"))) {
        copy_line(data, line);
        int expected = PHASE_STARTING;
        if (__atomic_compare_exchange_n(&data->phase, &expected, PHASE_ERROR,
                                        false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        }
        return;
    }

    if (phase == PHASE_STOPPING && strstr(line, "PCAP saved:")) {
        copy_line(data, line);
        __atomic_store_n(&data->phase, PHASE_SAVED, __ATOMIC_RELEASE);
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        return;
    }

    if (phase == PHASE_STOPPING && strstr(line, "All operations stopped")) {
        copy_line(data, line);
        __atomic_store_n(&data->phase, PHASE_SAVED, __ATOMIC_RELEASE);
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
    }
}

static void draw_screen(screen_t *self)
{
    radio_sniffer_data_t *data = (radio_sniffer_data_t *)self->user_data;
    if (!data) return;

    int phase = __atomic_load_n(&data->phase, __ATOMIC_ACQUIRE);

    ui_clear();
    ui_draw_title("Radio Sniffer");

    if (phase == PHASE_CAPTURING || phase == PHASE_SAVED || phase == PHASE_ERROR) {
        if (data->line[0] != '\0') {
            ui_print(0, 2, data->line, UI_COLOR_HIGHLIGHT);
        }
    } else if (phase == PHASE_STOPPING) {
        ui_print_center(3, "Stopping...", UI_COLOR_HIGHLIGHT);
    } else {
        ui_print_center(3, "Starting...", UI_COLOR_HIGHLIGHT);
    }

    const char *status = "ESC: Stop";
    if (phase == PHASE_STOPPING) {
        status = "Please wait";
    } else if (phase == PHASE_SAVED || phase == PHASE_ERROR) {
        status = "ESC: Back";
    }
    ui_draw_status(status);
}

static void on_tick(screen_t *self)
{
    radio_sniffer_data_t *data = (radio_sniffer_data_t *)self->user_data;
    if (!data) return;

    if (__atomic_exchange_n(&data->needs_redraw, false, __ATOMIC_ACQ_REL)) {
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    radio_sniffer_data_t *data = (radio_sniffer_data_t *)self->user_data;
    if (!data) return;

    switch (key) {
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE: {
            int phase = __atomic_load_n(&data->phase, __ATOMIC_ACQUIRE);
            if (phase == PHASE_STARTING || phase == PHASE_CAPTURING) {
                /* Publish STOPPING before TX so a fast "PCAP saved:" is not dropped. */
                __atomic_store_n(&data->phase, PHASE_STOPPING, __ATOMIC_RELEASE);
                __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
                if (uart_send_command("stop") == ESP_OK) {
                    data->stop_sent = true;
                } else {
                    int expected = PHASE_STOPPING;
                    __atomic_compare_exchange_n(&data->phase, &expected, phase,
                                                false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
                    __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
                }
            } else if (phase == PHASE_SAVED) {
                screen_manager_pop();
            } else if (phase == PHASE_ERROR) {
                if (!data->stop_sent && uart_send_command("stop") == ESP_OK) {
                    data->stop_sent = true;
                }
                screen_manager_pop();
            }
            break;
        }

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

screen_t* radio_sniffer_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating radio sniffer screen...");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    radio_sniffer_data_t *data = calloc(1, sizeof(radio_sniffer_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->phase = PHASE_STARTING;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    uart_register_line_callback(uart_line_callback, data);
    if (uart_send_command("start_pcap radio") != ESP_OK) {
        copy_line(data, "UART send failed");
        data->phase = PHASE_ERROR;
    }

    draw_screen(screen);

    ESP_LOGI(TAG, "Radio sniffer screen created");
    return screen;
}
