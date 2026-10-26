#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen_manager.h"
#include "uart_handler.h"

static unsigned sent_count, push_count, pop_count;
static char commands[16][48];
static char center_text[64];
static void (*line_callback)(const char *, void *);
static void *line_data;
static bool immediate_busy_on_start;
static bool fail_next_stop_send;
static int64_t fake_now;
int64_t esp_timer_get_time(void) { return fake_now; }

screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
int screen_manager_push(screen_t *(*create)(void *), void *params)
{ (void)create; (void)params; ++push_count; return ESP_OK; }
int screen_manager_pop(void) { ++pop_count; return ESP_OK; }
screen_t *sniffer_results_screen_create(void *params) { (void)params; return NULL; }
screen_t *sniffer_probes_screen_create(void *params) { (void)params; return NULL; }
int uart_send_command(const char *cmd)
{
    assert(sent_count < 16);
    snprintf(commands[sent_count++], sizeof(commands[0]), "%s", cmd);
    if (immediate_busy_on_start && !strcmp(cmd, "start_sniffer")) {
        immediate_busy_on_start = false;
        if (line_callback) line_callback("[WFACTL1] {\"v\":1,\"type\":\"error\",\"command\":\"console\",\"code\":\"busy\",\"reason\":\"radio_lease\"}", line_data);
    }
    if (fail_next_stop_send && !strcmp(cmd, "stop")) {
        fail_next_stop_send = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}
void uart_register_line_callback(void (*cb)(const char *, void *), void *data)
{ line_callback = cb; line_data = data; }
void uart_clear_line_callback(void) { line_callback = NULL; line_data = NULL; }
void ui_clear(void) {}
void ui_draw_title(const char *title) { (void)title; }
void ui_print_center(int row, const char *text, int color)
{ (void)color; if (row == 3) snprintf(center_text, sizeof(center_text), "%s", text); }
void ui_draw_status(const char *text) { (void)text; }
void buzzer_beep_attack(void) {}
static void rx(const char *line) { assert(line_callback); line_callback(line, line_data); }

#ifdef TEST_GLOBAL
#include "../main/screens/global_sniffer_screen.c"
#else
#include "../main/screens/sniffer_screen.c"
#endif

int main(void)
{
    immediate_busy_on_start = true;
#ifdef TEST_GLOBAL
    screen_t *screen = global_sniffer_screen_create(NULL);
#else
    sniffer_screen_params_t *params = calloc(1, sizeof(*params));
    params->count = 1;
    params->networks = calloc(1, sizeof(wifi_network_t));
    screen_t *screen = sniffer_screen_create(params);
#endif
    assert(screen && line_callback);
    assert(!immediate_busy_on_start); // A start command must reach the registered callback.
    screen->on_tick(screen);
    assert(strcmp(center_text, "Monster radio busy") == 0);
    screen->on_key(screen, KEY_R);
    assert(push_count == 0);
    unsigned before_exit = sent_count;
    screen->on_key(screen, KEY_ESC);
    assert(pop_count == 1 && sent_count == before_exit);
    screen->on_destroy(screen);
    free(screen);

    // ESC before JanOS acknowledges start stays on screen until ownership is known.
#ifdef TEST_GLOBAL
    screen = global_sniffer_screen_create(NULL);
#else
    params = calloc(1, sizeof(*params));
    params->count = 1;
    params->networks = calloc(1, sizeof(wifi_network_t));
    screen = sniffer_screen_create(params);
#endif
    assert(screen);
    unsigned before_pending_exit = sent_count;
    screen->on_key(screen, KEY_ESC);
    assert(pop_count == 1 && sent_count == before_pending_exit);
    rx("Sniffer started - scanning networks...");
    screen->on_tick(screen);
    assert(pop_count == 2 && sent_count == before_pending_exit + 1);
    assert(strcmp(commands[sent_count - 1], "stop") == 0);
    screen->on_destroy(screen);
    free(screen);

    // A delayed refusal releases the pending exit without a global stop.
#ifdef TEST_GLOBAL
    screen = global_sniffer_screen_create(NULL);
#else
    params = calloc(1, sizeof(*params));
    params->count = 1;
    params->networks = calloc(1, sizeof(wifi_network_t));
    screen = sniffer_screen_create(params);
#endif
    screen->on_key(screen, KEY_ESC);
    before_pending_exit = sent_count;
    rx("[WFACTL1] {\"code\":\"busy\",\"reason\":\"radio_lease\"}");
    screen->on_tick(screen);
    assert(pop_count == 3 && sent_count == before_pending_exit);
    screen->on_destroy(screen);
    free(screen);

    // Missing acknowledgement becomes explicit; only S is an operator stop.
#ifdef TEST_GLOBAL
    screen = global_sniffer_screen_create(NULL);
#else
    params = calloc(1, sizeof(*params));
    params->count = 1;
    params->networks = calloc(1, sizeof(wifi_network_t));
    screen = sniffer_screen_create(params);
#endif
    screen->on_key(screen, KEY_ESC);
    before_pending_exit = sent_count;
    fake_now += 6000000;
    screen->on_tick(screen);
    assert(strcmp(center_text, "Monster state unknown") == 0);
    assert(pop_count == 3 && sent_count == before_pending_exit);
    screen->on_key(screen, KEY_ESC);
    assert(pop_count == 3 && sent_count == before_pending_exit);
    fail_next_stop_send = true;
    screen->on_key(screen, KEY_S);
    assert(pop_count == 3 && sent_count == before_pending_exit + 1);
    assert(strcmp(center_text, "Stop send failed") == 0);
    screen->on_key(screen, KEY_S);
    assert(pop_count == 4 && sent_count == before_pending_exit + 2);
    assert(strcmp(commands[sent_count - 1], "stop") == 0);
    screen->on_destroy(screen);
    free(screen);

#ifdef TEST_GLOBAL
    // Results/Probes overlay leaves global capture running; resume must not
    // issue a second start, and ESC must still stop the active sniffer.
    screen = global_sniffer_screen_create(NULL);
    rx("Sniffer started - scanning networks...");
    screen->on_key(screen, KEY_R);
    assert(push_count == 1);
    unsigned before_resume = sent_count;
    screen->on_resume(screen);
    assert(sent_count == before_resume);
    screen->on_key(screen, KEY_ESC);
    assert(sent_count == before_resume + 1);
    assert(strcmp(commands[sent_count - 1], "stop") == 0);
    screen->on_destroy(screen);
    free(screen);
#else
    // Results/Probes must retain the owning screen if stop could not be sent.
    params = calloc(1, sizeof(*params));
    params->count = 1;
    params->networks = calloc(1, sizeof(wifi_network_t));
    screen = sniffer_screen_create(params);
    rx("Sniffer: Now monitoring selected networks (no scan performed)");
    fail_next_stop_send = true;
    screen->on_key(screen, KEY_R);
    assert(push_count == 0);
    assert(strcmp(center_text, "Stop send failed") == 0);
    fail_next_stop_send = true;
    screen->on_key(screen, KEY_P);
    assert(push_count == 0);
    screen->on_key(screen, KEY_ESC);
    assert(pop_count == 5);
    assert(strcmp(commands[sent_count - 1], "stop") == 0);
    screen->on_destroy(screen);
    free(screen);
#endif
    puts("Sniffer busy screen PASS");
    return 0;
}
