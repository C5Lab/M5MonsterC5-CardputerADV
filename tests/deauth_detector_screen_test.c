/* Exercise the production parser through its registered UART callback. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../main/screens/deauth_detector_screen.c"

static void (*rx_callback)(const char *, void *);
static void *rx_data;
static unsigned draws;
static char last_command[48];
static char commands[32][48];
static int command_count;
static int pops;
static char center_row[8][40];
static int last_progress;
screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
void screen_manager_pop(void) { pops++; }
void uart_send_command(const char *command) {
    snprintf(last_command, sizeof(last_command), "%s", command);
    if (command_count < 32) {
        snprintf(commands[command_count], sizeof(commands[0]), "%s", command);
        command_count++;
    }
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
void ui_print_center(int y, const char *s, int color) {
    (void)color;
    if (y >= 0 && y < 8 && s) snprintf(center_row[y], sizeof(center_row[y]), "%s", s);
}
void ui_draw_status(const char *s) { (void)s; }
void ui_draw_progress(int row, int progress, const char *text) {
    (void)row; (void)text;
    last_progress = progress;
}

static void rx(const char *line)
{
    assert(rx_callback);
    rx_callback(line, rx_data);
}

static void deauth_frame(int channel, const char *ap, const char *bssid, int rssi)
{
    char line[160];
    snprintf(line, sizeof(line),
             "[DEAUTH] CH: %d | AP: %s (%s) | RSSI: %d",
             channel, ap, bssid, rssi);
    rx(line);
}

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

    assert(strcmp(proximity_label(-16), "HERE") == 0);
    assert(strcmp(proximity_label(-35), "HERE") == 0);
    assert(strcmp(proximity_label(-36), "CLOSE") == 0);
    assert(strcmp(proximity_label(-50), "CLOSE") == 0);
    assert(strcmp(proximity_label(-51), "NEAR") == 0);
    assert(strcmp(proximity_label(-65), "NEAR") == 0);
    assert(strcmp(proximity_label(-66), "FAR") == 0);
    assert(strcmp(proximity_label(-80), "FAR") == 0);
    assert(strcmp(proximity_label(-81), "VERY FAR") == 0);
    assert(proximity_percent(-16) == 100);
    assert(proximity_percent(-20) == 100);
    assert(proximity_percent(-21) == 98);
    assert(proximity_percent(-55) == 50);
    assert(proximity_percent(-90) == 0);
    assert(proximity_percent(-91) == 0);

    /* Index 5 is the scan field, and hops across channels do not add up. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    int mark = command_count;
    rx("\"1\",\"\",\"\",\"C4:2B:44:12:29:21\",\"1\",\"WPA2\",\"-43\",\"2.4GHz\"");
    rx("\"5\",\"\"\"Home\"\"\",\"\",\"AC:22:05:83:C7:3F\",\"1\",\"WPA2\",\"-62\",\"2.4GHz\"");
    assert(data->scan_count == 2);
    assert(data->scan[1].index == 5);
    assert(strcmp(data->scan[1].bssid, "AC:22:05:83:C7:3F") == 0);

    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -16);
    deauth_frame(2, "\"Home\"", "AC:22:05:83:C7:3F", -16);
    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -16);
    screen->on_tick(screen);
    assert(data->phase == PHASE_SCAN);
    assert(data->best_channel == 1 && data->best_count == 2);
    assert(command_count == mark);

    deauth_frame(1, "\"Home\"", "ac:22:05:83:c7:3f", -16);
    assert(data->phase == PHASE_STOPPING && data->lock_index == 5);
    screen->on_tick(screen);
    assert(strcmp(commands[mark], "stop") == 0);

    last_progress = -1;
    center_row[4][0] = '\0';
    rx("All operations stopped.");
    screen->on_tick(screen);
    assert(data->phase == PHASE_TRACKING);
    assert(data->rssi == -16 && data->trend == TREND_NONE);
    assert(strcmp(commands[mark + 1], "deauth_detector 5") == 0);
    assert(strcmp(center_row[4], "HERE") == 0);
    assert(last_progress == 100);

    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -20);
    screen->on_tick(screen);
    assert(data->trend == TREND_FARTHER);
    assert(data->rssi == -20);
    assert(strcmp(center_row[4], "HERE") == 0);

    int pops_before = pops;
    int commands_before_esc = command_count;
    screen->on_key(screen, KEY_ESC);
    assert(pops == pops_before + 1);
    assert(strcmp(last_command, "stop") == 0);
    screen->on_tick(screen);
    assert(command_count == commands_before_esc + 1);
    screen->on_destroy(screen);
    free(screen);

    /* ESC while stop is in flight must not send the selected-mode command. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    mark = command_count;
    rx("\"5\",\"\"\"Home\"\"\",\"\",\"AC:22:05:83:C7:3F\",\"1\",\"WPA2\",\"-62\",\"2.4GHz\"");
    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -40);
    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -40);
    deauth_frame(1, "\"Home\"", "AC:22:05:83:C7:3F", -40);
    screen->on_tick(screen);
    assert(strcmp(last_command, "stop") == 0);
    screen->on_key(screen, KEY_ESC);
    rx("All operations stopped.");
    screen->on_tick(screen);
    for (int i = mark; i < command_count; i++) {
        assert(strncmp(commands[i], "deauth_detector", 15) != 0);
    }
    screen->on_destroy(screen);
    free(screen);

    /* Three frames for a BSSID that was not in the scan stay in scan mode. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    mark = command_count;
    deauth_frame(6, "Ghost", "02:00:00:00:00:99", -70);
    deauth_frame(6, "Ghost", "02:00:00:00:00:99", -70);
    deauth_frame(6, "Ghost", "02:00:00:00:00:99", -70);
    screen->on_tick(screen);
    assert(data->phase == PHASE_SCAN);
    assert(data->ap_missing);
    assert(command_count == mark);
    screen->on_destroy(screen);
    free(screen);

    puts("Deauth detector parser PASS");
    return 0;
}
