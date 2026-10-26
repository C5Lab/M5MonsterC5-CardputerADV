#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen_manager.h"
#include "uart_handler.h"
#include "esp_timer.h"

static uart_scan_complete_callback_t pending_scan;
static void *pending_data;
static char center_text[64];
static int scanning;
static int replaced;
static int popped;
static char scan_status[64] = "Starting scan...";

screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
int screen_manager_replace(screen_t *(*create)(void *), void *params)
{ (void)create; (void)params; ++replaced; return ESP_OK; }
int screen_manager_pop(void) { ++popped; return ESP_OK; }
screen_t *network_list_screen_create(void *params) { (void)params; return NULL; }
int uart_start_wifi_scan(uart_scan_complete_callback_t cb, void *data)
{ pending_scan = cb; pending_data = data; scanning = 1; return ESP_OK; }
bool uart_is_scanning(void) { return scanning != 0; }
const char *uart_get_scan_status(void) { return scan_status; }
void ui_clear(void) {}
void ui_draw_title(const char *title) { (void)title; }
void ui_print_center(int row, const char *text, int color)
{ (void)row; (void)color; snprintf(center_text, sizeof(center_text), "%s", text); }
void ui_draw_status(const char *text) { (void)text; }
void display_fill_rect(int x, int y, int w, int h, int color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; }
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{ *handle = (void *)args; return ESP_OK; }
int esp_timer_start_periodic(esp_timer_handle_t handle, int us)
{ (void)handle; (void)us; return ESP_OK; }
int esp_timer_stop(esp_timer_handle_t handle) { (void)handle; return ESP_OK; }
int esp_timer_delete(esp_timer_handle_t handle) { (void)handle; return ESP_OK; }

#include "../main/screens/wifi_scan_screen.c"

int main(void)
{
    screen_t *screen = wifi_scan_screen_create(NULL);
    assert(screen && pending_scan);
    scanning = 0;
    snprintf(scan_status, sizeof(scan_status), "Monster radio busy");
    pending_scan(NULL, 0, pending_data);
    update_timer_callback(screen->user_data);
    assert(strcmp(center_text, "Monster radio busy") == 0);
    assert(replaced == 0);
    screen->on_key(screen, KEY_ESC);
    assert(popped == 1);
    screen->on_destroy(screen);
    free(screen);

    screen = wifi_scan_screen_create(NULL);
    assert(screen && pending_scan);
    scanning = 0;
    snprintf(scan_status, sizeof(scan_status), "Monster state unknown");
    pending_scan(NULL, 0, pending_data);
    update_timer_callback(screen->user_data);
    assert(strcmp(center_text, "Monster state unknown") == 0);
    screen->on_destroy(screen);
    free(screen);
    puts("WiFi scan busy screen PASS");
    return 0;
}
