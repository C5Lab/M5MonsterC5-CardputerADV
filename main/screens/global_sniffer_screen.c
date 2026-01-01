/**
 * @file global_sniffer_screen.c
 * @brief Global sniffer screen implementation (no network selection required)
 */

#include "global_sniffer_screen.h"
#include "sniffer_results_screen.h"
#include "sniffer_probes_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GLOBAL_SNIFF";

// Screen user data
typedef struct {
    int packet_count;
    bool needs_redraw;
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
    if (!data) return;
    
    // Parse: "Sniffer packet count: N"
    const char *marker = "Sniffer packet count: ";
    const char *found = strstr(line, marker);
    
    if (found) {
        int count = atoi(found + strlen(marker));
        if (count != data->packet_count) {
            data->packet_count = count;
            data->needs_redraw = true;
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
    snprintf(count_str, sizeof(count_str), "Packets: %d", data->packet_count);
    ui_print_center(3, count_str, UI_COLOR_HIGHLIGHT);
    
    // Draw navigation hints
    ui_print_center(5, "R: Results  P: Probes", UI_COLOR_TEXT);
    
    // Draw status bar
    ui_draw_status("ESC: Stop");
}

static void on_tick(screen_t *self)
{
    global_sniffer_data_t *data = (global_sniffer_data_t *)self->user_data;
    
    // Check if redraw needed from UART callback (thread-safe)
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    ESP_LOGI(TAG, "Global sniffer on_key received: %d", (int)key);
    
    // Also check redraw on any key press for faster response
    on_tick(self);
    
    switch (key) {
        case KEY_R:
            // Show results (sniffer keeps running)
            screen_manager_push(sniffer_results_screen_create, NULL);
            break;
            
        case KEY_P:
            // Show probes (sniffer keeps running)
            screen_manager_push(sniffer_probes_screen_create, NULL);
            break;
            
        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            // Send stop command and go back to menu
            uart_send_command("stop");
            screen_manager_pop();
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
    
    // Send start_sniffer command directly (no network selection)
    uart_send_command("start_sniffer");
    
    // Draw initial screen
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Global sniffer screen created");
    return screen;
}

