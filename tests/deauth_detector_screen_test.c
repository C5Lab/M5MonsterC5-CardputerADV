/* Exercise the production parser through its registered UART callback. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../main/screens/deauth_detector_screen.c"

static void (*rx_callback)(const char *, void *);
static void *rx_data;
static unsigned draws;
static char last_command[32];
screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
void screen_manager_pop(void) {}
void uart_send_command(const char *command) {
    snprintf(last_command, sizeof(last_command), "%s", command);
}
void uart_register_line_callback(void (*callback)(const char *, void *), void *data) {
    rx_callback = callback; rx_data = data;
}
void uart_clear_line_callback(void) { rx_callback = NULL; rx_data = NULL; }
void ui_clear(void) { draws++; }
void ui_draw_title(const char *s) { (void)s; }
void ui_print(int x, int y, const char *s, int color) {
    (void)x; (void)y; (void)s; (void)color;
}
void ui_print_center(int y, const char *s, int color) { (void)y; (void)s; (void)color; }
void ui_draw_status(const char *s) { (void)s; }

static void reject(screen_t *screen, const char *line) {
    deauth_detector_data_t *data = screen->user_data;
    unsigned before_draws = draws;
    unsigned char before[sizeof(*data)];
    memcpy(before, data, sizeof(before));
    rx_callback(line, rx_data);
    assert(memcmp(before, data, sizeof(before)) == 0);
    screen->on_tick(screen);
    assert(draws == before_draws);
}

int main(void) {
    screen_t *screen = deauth_detector_screen_create(NULL);
    assert(screen && rx_callback);
    assert(strcmp(last_command, "deauth_detector") == 0);
    deauth_detector_data_t *data = screen->user_data;
    const char *startup = "Output: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>";
    reject(screen, startup);
    assert(!data->has_detection && data->detection_count == 0);

    rx_callback("[DEAUTH] CH: 6 | AP: Lab (upstairs) AP (02:Ab:00:00:00:01) | RSSI: -47", rx_data);
    assert(data->has_detection && data->channel == 6 && data->rssi == -47);
    assert(strcmp(data->ap_name, "Lab (upstairs) AP") == 0);
    assert(strcmp(data->bssid, "02:Ab:00:00:00:01") == 0);
    assert(data->detection_count == 1 && data->needs_redraw);
    screen->on_tick(screen);
    assert(!data->needs_redraw);

    const char *invalid[] = {
        "Output: [DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>",
        "[DEAUTH] CH: 0 | AP: Lab (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: -1 | AP: Lab (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: 999999999999999999999999 | AP: Lab (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: 6x | AP: Lab (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (<bssid>) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:0Z) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (02-00-00-00-00-01) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:001) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: <rssi>",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: -",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: -999999999999999999999999",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: 2147483648",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: -47junk",
        "[DEAUTH] CH: 6 | AP:  (02:00:00:00:00:01) | RSSI: -47",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01)",
        "[DEAUTH] CH: 6 | AP: Lab (02:00:00:00:00:01) | RSSI: ",
        "[DEAUTH] CH: 6 | AP: Lab (",
        "[DEAUTH] CH: ", "", NULL,
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) reject(screen, invalid[i]);
    reject(screen, startup);

    rx_callback("[DEAUTH] CH: 149 | AP: <Unknown> (02:00:00:00:00:02) | RSSI: -82", rx_data);
    assert(data->channel == 149 && data->rssi == -82 && data->detection_count == 2);
    assert(strcmp(data->ap_name, "<Unknown>") == 0);
    screen->on_tick(screen);
    rx_callback("[DEAUTH] CH: 36 | AP: AP ) | RSSI: fake (room) (02:00:00:00:00:03) | RSSI: -50", rx_data);
    assert(strcmp(data->ap_name, "AP ) | RSSI: fake (room)") == 0);
    assert(data->channel == 36 && data->detection_count == 3);
    screen->on_tick(screen);

    screen->on_destroy(screen);
    free(screen);
    assert(!rx_callback);

    // A real detection arriving first must not be discarded as startup output.
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    rx_callback("[DEAUTH] CH: 1 | AP: Lab (02:00:00:00:00:01) | RSSI: -30", rx_data);
    assert(data->has_detection && data->detection_count == 1);
    screen->on_destroy(screen);
    free(screen);
    puts("Deauth detector parser PASS");
    return 0;
}
