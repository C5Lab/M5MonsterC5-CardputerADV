#include "screen_manager.h"
#include "uart_handler.h"
#include "text_ui.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now;
static unsigned sent_count, pop_count;
static char sent[32][96], footer[40], result_row[40];
static int stop_result = ESP_OK;
static bool verified_stop_supported;
static bool dispatching;
static void (*callback)(const char *, void *);
static void *callback_data;

int64_t esp_timer_get_time(void) { return now; }
screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
int screen_manager_pop(void) { assert(!dispatching); ++pop_count; return ESP_OK; }
void uart_register_line_callback(void (*cb)(const char *, void *), void *data) {
    assert(!dispatching); callback = cb; callback_data = data;
}
void uart_clear_line_callback(void) { assert(!dispatching); callback = NULL; callback_data = NULL; }
int uart_send_command(const char *command) {
    assert(sent_count < 32);
    snprintf(sent[sent_count++], sizeof(sent[0]), "%s", command);
    return strcmp(command, "wps stop") == 0 ? stop_result : ESP_OK;
}
bool uart_janos_version_at_least(int major, int minor, int patch) {
    assert(major == 1 && minor == 7 && patch == 7);
    return verified_stop_supported;
}
void buzzer_beep_attack(void) {}
void ui_clear(void) {}
void ui_draw_title(const char *s) { (void)s; }
void ui_print(int x, int y, const char *s, int color) {
    (void)x; (void)color;
    if (y == 4) snprintf(result_row, sizeof(result_row), "%s", s);
}
void ui_draw_status(const char *s) { snprintf(footer, sizeof(footer), "%s", s); }
static void rx(const char *line) {
    assert(callback); dispatching = true; callback(line, callback_data); dispatching = false;
}

#include "../main/screens/pixie_dust_screen.c"

static screen_t *make_screen(void) {
    pixie_dust_screen_params_t *params = calloc(1, sizeof(*params));
    assert(params);
    snprintf(params->network.ssid, sizeof(params->network.ssid), "Lab AP");
    snprintf(params->network.bssid, sizeof(params->network.bssid), "02:00:00:00:00:01");
    screen_t *screen = pixie_dust_screen_create(params);
    assert(screen);
    assert(!strcmp(sent[sent_count - 1], "wps pixie 02:00:00:00:00:01"));
    return screen;
}
static void destroy_screen(screen_t *screen) {
    screen->on_destroy(screen);
    free(screen);
    assert(!callback);
}
static void reset_fixture(void) {
    now = 0; sent_count = 0; pop_count = 0; stop_result = ESP_OK;
    verified_stop_supported = true;
    footer[0] = '\0'; result_row[0] = '\0';
    assert(!callback);
}

static void stop_waits_for_confirmation(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":7,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 0);
    assert(sent_count == 2 && !strcmp(sent[1], "wps stop"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stop_pending\",\"phase\":\"cleanup\"}");
    screen->on_tick(screen);
    assert(pop_count == 0);
    now += STATUS_POLL_US;
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps status"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"idle\",\"run_id\":7,\"generation\":7,\"operation_result\":\"cancelled\",\"cleanup_result\":\"restored\",\"bulk_bytes\":0,\"bulk_peak_bytes\":0,\"secret_bytes\":0,\"secret_peak_bytes\":0,\"allocation_failures\":0}");
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps stop"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stopped\",\"phase\":\"idle\"}");
    screen->on_tick(screen);
    assert(pop_count == 1);
    destroy_screen(screen);
}

static void timeout_does_not_silently_abandon_accepted_run(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    now = START_TIMEOUT_US + 1;
    screen->on_tick(screen);
    assert(pop_count == 0);
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":8,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 0 && !strcmp(sent[sent_count - 1], "wps stop"));
    destroy_screen(screen);
}

static void unknown_run_needs_explicit_stop(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    now = START_TIMEOUT_US + 1;
    screen->on_tick(screen);
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(sent_count == 1); /* No global stop without an accepted run_id. */
    assert(pop_count == 0); /* First ESC shows the uncertain state. */
    screen->on_key(screen, KEY_ENTER);
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps stop"));
    destroy_screen(screen);
}

static void unrelated_terminal_cannot_end_our_run(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":9,\"operation\":\"wps_pixie\"}");
    rx("{\"schema_version\":1,\"record_type\":\"terminal\",\"run_id\":10,\"operation_result\":\"success\",\"cleanup_result\":\"restored\",\"lease_state\":\"released\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 0 && !strcmp(sent[sent_count - 1], "wps stop"));
    destroy_screen(screen);
}

static void terminal_before_stop_reply_cannot_release_screen(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":11,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    rx("{\"schema_version\":1,\"record_type\":\"terminal\",\"run_id\":11,\"operation_result\":\"cancelled\",\"cleanup_result\":\"restored\",\"lease_state\":\"released\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 0); /* Worker epilogue may still be active. */
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stopped\",\"phase\":\"idle\"}");
    screen->on_tick(screen);
    assert(pop_count == 1);
    destroy_screen(screen);
}

static void legacy_stop_reply_is_unconfirmed(void) {
    reset_fixture();
    verified_stop_supported = false;
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":12,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stopped\",\"phase\":\"idle\"}");
    screen->on_tick(screen);
    assert(pop_count == 0 && strstr(result_row, "unconfirmed"));
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 1); /* Explicit leave after warning. */
    destroy_screen(screen);
}

static void changed_run_id_blocks_stop_retry(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":13,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stop_pending\",\"phase\":\"cleanup\"}");
    now += STATUS_POLL_US;
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps status"));
    unsigned before = sent_count;
    rx("{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"running\",\"run_id\":14,\"generation\":14,\"operation_result\":\"none\",\"cleanup_result\":\"not_required\",\"bulk_bytes\":0,\"bulk_peak_bytes\":0,\"secret_bytes\":0,\"secret_peak_bytes\":0,\"allocation_failures\":0}");
    screen->on_tick(screen);
    assert(sent_count == before && pop_count == 0);
    assert(strstr(result_row, "unconfirmed"));
    destroy_screen(screen);
}

static void failed_stop_tx_can_be_retried_after_status(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":15,\"operation\":\"wps_pixie\"}");
    stop_result = ESP_FAIL;
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 0 && strstr(result_row, "send failed"));
    stop_result = ESP_OK;
    screen->on_key(screen, KEY_ENTER);
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps status"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"running\",\"run_id\":15,\"generation\":15,\"operation_result\":\"none\",\"cleanup_result\":\"not_required\",\"bulk_bytes\":0,\"bulk_peak_bytes\":0,\"secret_bytes\":0,\"secret_peak_bytes\":0,\"allocation_failures\":0}");
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps stop"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stopped\",\"phase\":\"idle\"}");
    screen->on_tick(screen);
    assert(pop_count == 1);
    destroy_screen(screen);
}

static void quarantine_is_visible_and_requires_explicit_leave(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":16,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"quarantined\",\"phase\":\"quarantined\"}");
    screen->on_tick(screen);
    assert(pop_count == 0 && strstr(result_row, "recovery"));
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    assert(pop_count == 1);
    destroy_screen(screen);
}

static void late_accept_during_manual_stop_waits_for_status_before_retry(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    now = START_TIMEOUT_US + 1;
    screen->on_tick(screen);
    screen->on_key(screen, KEY_ENTER); /* Explicit global stop after missing ACK. */
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps stop"));
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":17,\"operation\":\"wps_pixie\"}");
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stop_pending\",\"phase\":\"cleanup\"}");
    unsigned before = sent_count;
    screen->on_tick(screen);
    assert(sent_count == before); /* No second global stop without run check. */
    destroy_screen(screen);
}

static void unsolicited_secret_is_not_displayed(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":18,\"operation\":\"wps_pixie\"}");
    rx("SENSITIVE recovered_pin=12345670 network_key=\"wrong-network\"");
    pixie_data_t *data = screen->user_data;
    assert(data->pin[0] == '\0' && data->psk[0] == '\0');
    destroy_screen(screen);
}

static void only_our_stage_changes_progress(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":19,\"operation\":\"wps_pixie\"}");
    pixie_data_t *data = screen->user_data;
    assert(data->phase == PIXIE_CAPTURE && !data->found);
    rx("{\"schema\":1,\"operation_id\":20,\"type\":\"wps_stage\",\"stage\":\"pixie_done\",\"found\":true}");
    assert(data->phase == PIXIE_CAPTURE && !data->found);
    rx("{\"schema\":1,\"type\":\"wps_stage\",\"stage\":\"pixie_done\",\"found\":true}");
    assert(data->phase == PIXIE_CAPTURE && !data->found);
    rx("{\"schema\":1,\"operation_id\":19,\"type\":\"wps_stage\",\"stage\":\"pixie_done\",\"found\":true}");
    assert(data->phase == PIXIE_SEARCH && data->found);
    destroy_screen(screen);
}

static void revealed_secret_must_match_accepted_run(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":21,\"operation\":\"wps_pixie\"}");
    rx("{\"schema_version\":1,\"record_type\":\"terminal\",\"run_id\":21,\"operation_result\":\"pixie_pin_and_m7_verified\",\"cleanup_result\":\"restored\",\"lease_state\":\"released\"}");
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "secret reveal"));
    pixie_data_t *data = screen->user_data;
    rx("SENSITIVE operation_id=22 recovered_pin=12345670 network_key=\"other-network\"");
    assert(data->pin[0] == '\0' && data->psk[0] == '\0');
    rx("SENSITIVE recovered_pin=12345670 network_key=\"missing-id\"");
    assert(data->pin[0] == '\0' && data->psk[0] == '\0');
    rx("SENSITIVE operation_id=21 recovered_pin=87654321 network_key=\"our-network\"");
    assert(!strcmp(data->pin, "87654321") && !strcmp(data->psk, "our-network"));
    destroy_screen(screen);
}

static void explicit_retry_gets_new_stop_window(void) {
    reset_fixture();
    screen_t *screen = make_screen();
    rx("{\"schema_version\":1,\"record_type\":\"accepted\",\"run_id\":23,\"operation\":\"wps_pixie\"}");
    screen->on_key(screen, KEY_ESC);
    screen->on_tick(screen);
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stop_pending\",\"phase\":\"cleanup\"}");
    now = STOP_TOTAL_TIMEOUT_US + 1;
    screen->on_tick(screen);
    pixie_data_t *data = screen->user_data;
    assert(data->phase == PIXIE_UNKNOWN);
    screen->on_key(screen, KEY_ENTER);
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps status"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_status\",\"phase\":\"running\",\"run_id\":23,\"generation\":23,\"operation_result\":\"none\",\"cleanup_result\":\"not_required\"}");
    screen->on_tick(screen);
    assert(!strcmp(sent[sent_count - 1], "wps stop"));
    rx("{\"schema_version\":1,\"record_type\":\"wps_stop\",\"result\":\"stop_pending\",\"phase\":\"cleanup\"}");
    now += STATUS_POLL_US;
    screen->on_tick(screen);
    assert(data->phase == PIXIE_STOPPING);
    assert(!strcmp(sent[sent_count - 1], "wps status"));
    destroy_screen(screen);
}

int main(void) {
    stop_waits_for_confirmation();
    timeout_does_not_silently_abandon_accepted_run();
    unknown_run_needs_explicit_stop();
    unrelated_terminal_cannot_end_our_run();
    terminal_before_stop_reply_cannot_release_screen();
    legacy_stop_reply_is_unconfirmed();
    changed_run_id_blocks_stop_retry();
    failed_stop_tx_can_be_retried_after_status();
    quarantine_is_visible_and_requires_explicit_leave();
    late_accept_during_manual_stop_waits_for_status_before_retry();
    unsolicited_secret_is_not_displayed();
    only_our_stage_changes_progress();
    revealed_secret_must_match_accepted_run();
    explicit_retry_gets_new_stop_window();
    puts("Pixie screen lifecycle PASS");
    return 0;
}
