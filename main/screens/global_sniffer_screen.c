/**
 * @file global_sniffer_screen.c
 * @brief Global sniffer screen implementation (no network selection required)
 */

#include "global_sniffer_screen.h"
#include "sniffer_results_screen.h"
#include "sniffer_probes_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GLOBAL_SNIFF";
#define SNIFFER_ACK_TIMEOUT_US 5000000
enum { START_PENDING, STARTED, START_BUSY, START_REFUSED };

// Screen user data
typedef struct {
    int packet_count;
    bool needs_redraw; // atomic handoff from UART callback to UI tick
    int start_state;    // atomic: UART callback writes, UI reads
    bool exit_requested;
    bool state_unknown;
    bool stop_send_failed;
    int64_t start_deadline_us;
    screen_t *self;
} global_sniffer_data_t;

// Forward declaration
static void draw_screen(screen_t *self);

/**
 * @brief UART line callback for parsing sniffer output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)user_data;
    if (!data || !line) return;

    int state = __atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE);
    if (state == START_BUSY || state == START_REFUSED) return;
    if (state == START_PENDING && strstr(line, "\"code\":\"busy\"") &&
        strstr(line, "\"reason\":\"radio_lease\"")) {
        __atomic_store_n(&data->start_state, START_BUSY, __ATOMIC_RELEASE);
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        return;
    }
    if (state == START_PENDING &&
        (strstr(line, "Failed to start scan for sniffer") ||
         strstr(line, "Failed to initialize selected networks for sniffer") ||
         strstr(line, "Sniffer already active"))) {
        __atomic_store_n(&data->start_state, START_REFUSED, __ATOMIC_RELEASE);
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        return;
    }
    if (state == START_PENDING &&
        (strstr(line, "Sniffer started") || strstr(line, "Sniffer: Now monitoring"))) {
        __atomic_store_n(&data->start_state, STARTED, __ATOMIC_RELEASE);
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
    }
    
    // Parse: "Sniffer packet count: N"
    const char *marker = "Sniffer packet count: ";
    const char *found = strstr(line, marker);
    
    if (found) {
        __atomic_store_n(&data->start_state, STARTED, __ATOMIC_RELEASE);
        int count = atoi(found + strlen(marker));
        if (count != __atomic_load_n(&data->packet_count, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&data->packet_count, count, __ATOMIC_RELEASE);
            __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
        }
    }
}

static void draw_screen(screen_t *self)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    ui_clear();
    
    // Draw title
    ui_draw_title("Sniffer Running");
    
    // Draw packet count in center
    char count_str[32];
    int state = __atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE);
    snprintf(count_str, sizeof(count_str), "Packets: %d",
             __atomic_load_n(&data->packet_count, __ATOMIC_ACQUIRE));
    const char *message = data->stop_send_failed ? "Stop send failed" :
                          state == START_BUSY ? "Monster radio busy" :
                          state == START_REFUSED ? "Sniffer refused" :
                          state == STARTED ? count_str :
                          data->state_unknown ? "Monster state unknown" :
                          data->exit_requested ? "Waiting for Monster..." :
                                                 "Starting sniffer...";
    ui_print_center(3, message,
                    state == STARTED ? UI_COLOR_HIGHLIGHT : UI_COLOR_TEXT);
    
    // Draw navigation hints
    if (state == STARTED)
        ui_print_center(5, "R: Results  P: Probes", UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status(data->stop_send_failed ?
                   (state == STARTED ? "ESC: Retry stop" : "S: Retry send stop") :
                   state == STARTED ? "ESC: Stop" :
                   data->state_unknown ? "S: Send stop + back" :
                   data->exit_requested ? "ESC: Waiting" : "ESC: Back");
}

static void on_tick(screen_t *self)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback (thread-safe)
    int state = __atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE);
    if (data->exit_requested && state == STARTED) {
        if (uart_send_command("stop") == ESP_OK) {
            screen_manager_pop();
        } else {
            data->exit_requested = false;
            data->stop_send_failed = true;
            draw_screen(self);
        }
        return;
    }
    if (data->exit_requested && (state == START_BUSY || state == START_REFUSED)) {
        screen_manager_pop();
        return;
    }
    if (state == START_PENDING && !data->state_unknown &&
        esp_timer_get_time() >= data->start_deadline_us) {
        data->state_unknown = true;
        __atomic_store_n(&data->needs_redraw, true, __ATOMIC_RELEASE);
    }
    if (state != START_PENDING) data->state_unknown = false;
    if (__atomic_exchange_n(&data->needs_redraw, false, __ATOMIC_ACQ_REL)) {
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    ESP_LOGI(TAG, "Global sniffer on_key received: %d", (int)key);
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    switch (key) {
        case KEY_R:
            if (__atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE) != STARTED) break;
            // Show results (sniffer keeps running)
            screen_manager_push(sniffer_results_screen_create, NULL);
            break;
            
        case KEY_P:
            if (__atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE) != STARTED) break;
            // Show probes (sniffer keeps running)
            screen_manager_push(sniffer_probes_screen_create, NULL);
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            {
                int state = __atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE);
                if (state == STARTED) {
                    if (uart_send_command("stop") == ESP_OK) {
                        screen_manager_pop();
                    } else {
                        data->stop_send_failed = true;
                        draw_screen(self);
                    }
                } else if (state == START_BUSY || state == START_REFUSED) {
                    screen_manager_pop();
                } else {
                    data->exit_requested = true;
                    draw_screen(self);
                }
            }
            break;

        case KEY_S:
            if (data->state_unknown &&
                __atomic_load_n(&data->start_state, __ATOMIC_ACQUIRE) == START_PENDING) {
                if (uart_send_command("stop") == ESP_OK) {
                    screen_manager_pop();
                } else {
                    data->stop_send_failed = true;
                    draw_screen(self);
                }
            }
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    // Clear UART callback
    uart_clear_line_callback();
    
    if (data) {
        free(data);
    }
}

static void on_resume(screen_t *self)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    // Re-register UART callback after returning from sub-screens
    uart_register_line_callback(uart_line_callback, data);
    // Results/Probes overlays leave this global sniffer running. Preserve its
    // confirmed ownership; a duplicate start would be refused by JanOS.
    
    // Redraw the screen
    draw_screen(self);
}

screen_t* global_sniffer_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating global sniffer screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    // Allocate user data
    global_sniffer_data_t *data = calloc(1, sizeof(global_sniffer_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->self = screen;
    data->packet_count = 0;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback for parsing sniffer output
    uart_register_line_callback(uart_line_callback, data);
    
    // Clear leftover scan selection so JanOS hops all channels, not one AP.
    data->start_deadline_us = esp_timer_get_time() + SNIFFER_ACK_TIMEOUT_US;
    uart_send_command("unselect_networks");
    if (uart_send_command("start_sniffer") != ESP_OK) data->state_unknown = true;
    buzzer_beep_attack();
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Global sniffer screen created");
    return screen;
}

