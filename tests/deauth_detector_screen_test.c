/* Host exercise of the real ADV detector screen and JanOS UART contract. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/screens/deauth_detector_screen.c"

static void (*rx_callback)(const char *, void *);
static void *rx_data;
static int64_t fake_us;
static int send_result;
static char commands[256][256];
static int command_count;
static int pops;
static char rows[8][96];
static int full_clears;
static int content_clears;
static int title_draws;

screen_t *screen_alloc(void) { return calloc(1, sizeof(screen_t)); }
void screen_manager_pop(void) { pops++; }
int64_t esp_timer_get_time(void) { return fake_us; }
int uart_send_command(const char *command)
{
    if (send_result) return send_result;
    assert(command_count < 256);
    snprintf(commands[command_count++], sizeof(commands[0]), "%s", command);
    return 0;
}
void uart_register_line_callback(void (*callback)(const char *, void *), void *data)
{ rx_callback = callback; rx_data = data; }
void uart_clear_line_callback(void) { rx_callback = NULL; rx_data = NULL; }
void ui_clear(void) { ++full_clears; memset(rows, 0, sizeof(rows)); }
void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)color;
    assert(x == 0 && y == 19 && w == DISPLAY_WIDTH && h == 98);
    ++content_clears;
    for (int row = 1; row <= 6; ++row) rows[row][0] = '\0';
}
void ui_draw_title(const char *s)
{
    ++title_draws;
    snprintf(rows[0], sizeof(rows[0]), "%s", s);
}
void ui_print(int x, int y, const char *s, int color)
{
    (void)x; (void)color;
    if (y >= 0 && y < 8) snprintf(rows[y], sizeof(rows[y]), "%s", s);
}
void ui_print_center(int y, const char *s, int color)
{
    (void)color;
    if (y >= 0 && y < 8) snprintf(rows[y], sizeof(rows[y]), "%s", s);
}
void ui_draw_status(const char *s) { snprintf(rows[7], sizeof(rows[7]), "%s", s); }
void ui_draw_progress(int row, int progress, const char *text)
{ (void)row; (void)progress; (void)text; }

static void rx(const char *line)
{
    assert(rx_callback);
    rx_callback(line, rx_data);
}
static void tick(screen_t *screen, int ms)
{
    fake_us += (int64_t)ms * 1000;
    screen->on_tick(screen);
}
static void key(screen_t *screen, key_code_t code) { screen->on_key(screen, code); }
static void close_screen(screen_t *screen)
{
    screen->on_destroy(screen);
    free(screen);
    assert(!rx_callback);
}

static bool ui_has(const char *text)
{
    for (int row = 0; row < 8; ++row) {
        if (strstr(rows[row], text)) return true;
    }
    return false;
}

static screen_t *choice_screen(void)
{
    return deauth_detector_screen_create(NULL);
}

static screen_t *started_screen(bool trace)
{
    int before = command_count;
    screen_t *screen = choice_screen();
    assert(screen && rx_callback);
    assert(command_count == before);
    screen->on_draw(screen);
    assert(ui_has("Trace") && ui_has("OFF") && ui_has("JanOS confirms"));
    if (trace) {
        key(screen, KEY_SPACE);
        screen->on_draw(screen);
        assert(ui_has("Trace") && ui_has("ON"));
    }
    key(screen, KEY_ENTER);
    assert(command_count == before + 1);
    return screen;
}

/* Existing tests exercise the default-off path unless they explicitly call
   started_screen(true) or choice_screen(). */
#define deauth_detector_screen_create(params) started_screen(false)

static void legacy_all_started(deauth_detector_data_t *data)
{
    assert(data->state == DET_START_ALL);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    rx("Unrecognized command");
    assert(data->state == DET_START_ALL);
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_ACTIVE_ALL);
}

static void legacy_selected_started(deauth_detector_data_t *data)
{
    assert(data->state == DET_START_SELECTED);
    assert(strstr(commands[command_count - 1], "deauth_detector_v2 --only ") == commands[command_count - 1]);
    rx("Unrecognized command");
    assert(data->state == DET_START_SELECTED);
    assert(strstr(commands[command_count - 1], "deauth_detector --only ") == commands[command_count - 1]);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
}

static void v2_all_records(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    deauth_detector_data_t *data = screen->user_data;
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    rx("Output: [DEAUTH_V2] CH: <ch> | BSSID: <mac> | TX: <mac> | DA: <mac> | RSSI: <rssi> | REASON: <reason> | SSID: <text>");
    rx("[DETECTOR_INFO] CSA/ECSA announcements enabled.");
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_START_ALL);
    assert(strstr(data->message, "format") != NULL);
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 65535 | SSID: Too early");
    assert(data->deauth_count == 0);
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_ALL);
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 65535 | SSID: Lab [upstairs] (A) | guest");
    tick(screen, 1);
    assert(data->deauth_count == 1 && data->csa_count == 0);
    assert(strcmp(data->last_deauth.ap_name, "Lab [upstairs] (A) | guest") == 0);
    screen->on_draw(screen);
    assert(strstr(rows[0], "DEAUTH 1") != NULL);
    assert(strstr(rows[1], "Lab [upstairs] (A)") != NULL);
    assert(strstr(rows[2], "Reason 65535") != NULL);
    assert(strstr(rows[3], "02:00:00:00:00:01") != NULL);
    assert(strstr(rows[4], "Claimed TX 02:00:00:00:00:03") != NULL);
    assert(strstr(rows[5], "Frame recipient / DA") != NULL);
    assert(strstr(rows[6], "no single recipient") != NULL);
    assert(strstr(rows[7], "W:Watch") == NULL);
    for (int row = 0; row <= 7; ++row) assert(strlen(rows[row]) <= 30);
    rx("[DEAUTH] CH: 6 | AP: Lab [upstairs] (A) | guest (02:00:00:00:00:01) | RSSI: -47");
    assert(data->deauth_count == 1);
    rx("Output: [DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: sample");
    rx("[DETECTOR_INFO] format=deauth_v2");
    rx("[DEAUTH_V2] CH: 0 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: not-a-mac | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | TX: 02:00:00:00:00:03 | BSSID: 02:00:00:00:00:01 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: bad | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: -1 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 65536 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 99999999999999999999 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 256 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: bad | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: bad | RSSI: -47 | REASON: 7 | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: - | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: 01:00:5E:00:00:01 | RSSI: -47 | REASON: - | SSID: Bad");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7");
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: More than thirty two characters in this SSID");
    assert(data->deauth_count == 1);
    assert(strcmp(data->last_deauth.ap_name, "Lab [upstairs] (A) | guest") == 0);
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Lab [upstairs]");
    assert(data->deauth_count == 1 && data->csa_count == 1);
    key(screen, KEY_C);
    screen->on_draw(screen);
    assert(strstr(rows[0], "CSA") && strstr(rows[2], "Lab [upstairs]"));
    key(screen, KEY_C);
    rx("[DEAUTH_V2] CH: 11 | BSSID: 02:00:00:00:00:02 | TX: 02:00:00:00:00:04 | DA: 02:00:00:00:00:05 | RSSI: -55 | REASON: 0 | SSID: Lab [B]");
    screen->on_draw(screen);
    assert(data->deauth_count == 2 && data->csa_count == 1);
    assert(strstr(rows[2], "Reason 0") != NULL);
    assert(strstr(rows[5], "Frame recipient / DA") != NULL);
    assert(strstr(rows[6], "02:00:00:00:00:05") != NULL);
    for (int row = 0; row <= 7; ++row) assert(strlen(rows[row]) <= 30);
    rx("[DEAUTH_V2] CH: 11 | BSSID: 02:00:00:00:00:02 | TX: 02:00:00:00:00:04 | DA: 02:00:00:00:00:05 | RSSI: -55 | REASON: - | SSID: Lab [B]");
    screen->on_draw(screen);
    assert(data->deauth_count == 3 && data->csa_count == 1);
    assert(strstr(rows[2], "Reason -") != NULL);
    assert(strstr(rows[2], "PMF") != NULL);
    int saved_deauth = data->deauth_count, saved_csa = data->csa_count;
    data->deauth_count = 10000;
    data->csa_count = 10000;
    screen->on_draw(screen);
    assert(strlen(rows[0]) <= 21 && strstr(rows[0], "DEAUTH") != NULL);
    assert(strstr(rows[0], "CSA") != NULL);
    data->deauth_count = saved_deauth;
    data->csa_count = saved_csa;
    int before = command_count, popped = pops;
    key(screen, KEY_Q);
    assert(command_count == before && pops == popped);
    key(screen, KEY_ESC);
    assert(data->state == DET_ACTIVE_ALL);
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("Deauth Detector stopped.");
    tick(screen, 1);
    assert(pops == popped);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);
}

static void prompt_prefixed_records(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    deauth_detector_data_t *data = screen->user_data;
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_ALL);

    /* JanOS can print its REPL prompt immediately before the first frame. */
    rx("> [CSA] RX: 11 | TO: 1 | BSSID: 84:A1:D1:C9:83:E0 | TX: 84:A1:D1:C9:83:E0 | RSSI: -87 | VIA: probe | COUNT: 3 | MODE: 0 | CLASS: 4 | SSID: Orange_Swiatlowod_83E0");
    assert(data->csa_count == 1 && data->deauth_count == 0);
    assert(data->last_csa.rx == 11 && data->last_csa.to == 1);
    assert(strcmp(data->last_csa.bssid, "84:A1:D1:C9:83:E0") == 0);

    rx("> [DEAUTH_V2] CH: 11 | BSSID: 84:A1:D1:C9:83:E0 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Orange_Swiatlowod_83E0");
    assert(data->csa_count == 1 && data->deauth_count == 1);
    assert(data->last_deauth.reason == 7);
    rx("Output: > [DEAUTH_V2] CH: 11 | BSSID: 84:A1:D1:C9:83:E0 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: Example");
    rx("noise > [CSA] RX: 11 | TO: 1 | BSSID: 84:A1:D1:C9:83:E0 | TX: 84:A1:D1:C9:83:E0 | RSSI: -87 | VIA: probe | COUNT: 3 | MODE: 0 | CLASS: 4 | SSID: Example");
    assert(data->csa_count == 1 && data->deauth_count == 1);
    rx(">[CSA] RX: 11 | TO: 1 | BSSID: 84:A1:D1:C9:83:E0 | TX: 84:A1:D1:C9:83:E0 | RSSI: -87 | VIA: probe | COUNT: 3 | MODE: 0 | CLASS: 4 | SSID: Example");
    assert(data->csa_count == 2 && data->deauth_count == 1);
    close_screen(screen);
}
static void enter_scan(screen_t *screen)
{
    deauth_detector_data_t *data = screen->user_data;
    if (data->state == DET_START_ALL)
        legacy_all_started(data);
    assert(data->state == DET_ACTIVE_ALL);
    key(screen, KEY_S);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    int commands_while_stopping = command_count;
    rx("Deauth Detector stopped.");
    assert(data->state == DET_STOP_FOR_SCAN && command_count == commands_while_stopping);
    rx("All operations stopped.");
    assert(strcmp(commands[command_count - 1], "scan_networks") == 0);
    assert(data->state == DET_SCAN_WAIT_ACK);
}
static void complete_scan_two(screen_t *screen)
{
    deauth_detector_data_t *data = screen->user_data;
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    /* No result set can be used until JanOS reports the new scan complete. */
    rx("Showing results from last scan (1 networks found):");
    rx("\"1\",\"stale\",\"\",\"02:00:00:00:00:FF\",\"1\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->state == DET_SCAN_RUNNING && data->scan_count == 0);
    int before = command_count;
    tick(screen, 1000);
    assert(command_count == before);
    rx("WiFi scan completed. Found 2 networks, status: 0");
    rx("Retrieved 2 network records in 4.2s");
    rx("\"1\",\"Home (upstairs)\",\"\",\"02:00:00:00:00:01\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("\"2\",\"Home (upstairs)\",\"\",\"02:00:00:00:00:02\",\"11\",\"WPA2\",\"-50\",\"2.4GHz\"");
    assert(data->state != DET_PICK);
    rx("Scan results printed.");
    assert(data->state == DET_PICK && data->scan_count == 2);
    assert(data->scan[0].index == 1 && data->scan[1].index == 2);
    assert(strcmp(data->scan[0].ssid, data->scan[1].ssid) == 0);
    assert(strcmp(data->scan[0].bssid, data->scan[1].bssid) != 0);
}

static void v2_selected_records(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    deauth_detector_data_t *data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1") == 0);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
    assert(data->state == DET_START_SELECTED);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home (upstairs)");
    assert(data->state == DET_START_SELECTED);
    assert(strstr(data->message, "format") != NULL);
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_SELECTED);
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:02 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -40 | REASON: 7 | SSID: Home (upstairs)");
    assert(data->deauth_count == 0);
    rx("[DEAUTH_V2] CH: 6 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -40 | REASON: 7 | SSID: Other [label]");
    assert(data->deauth_count == 1);
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:02 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Other");
    assert(data->csa_count == 0);
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Home");
    assert(data->csa_count == 1 && data->deauth_count == 1);
    screen->on_draw(screen);
    assert(strstr(rows[7], "W:Watch") != NULL);
    key(screen, KEY_W);
    screen->on_draw(screen);
    assert(strstr(rows[3], "02:00:00:00:00:01") != NULL);
    key(screen, KEY_S);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK);
    close_screen(screen);
}

static void v2_start_timeout(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    deauth_detector_data_t *data = screen->user_data;
    int before = command_count;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_START_ALL);
    tick(screen, 36000);
    assert(data->state == DET_ERROR && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    before = command_count;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_START_ALL);
    rx("Unrecognized command");
    assert(data->state == DET_ERROR && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    before = command_count;
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_START_ALL);
    rx("Unrecognized command");
    assert(data->state == DET_ERROR && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    before = command_count;
    rx("Unrecognized command: deauth_detector_v2");
    assert(data->state == DET_START_ALL && command_count == before);
    tick(screen, 36000);
    assert(data->state == DET_ERROR && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    before = command_count;
    tick(screen, 34000);
    rx("Unrecognized command");
    assert(data->state == DET_START_ALL && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
    tick(screen, 34000);
    assert(data->state == DET_START_ALL);
    rx("Unrecognized command");
    assert(data->state == DET_ERROR && command_count == before + 1);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    before = command_count;
    assert(strcmp(commands[before - 1], "deauth_detector_v2 --only 1") == 0);
    tick(screen, 9000);
    assert(data->state == DET_ERROR && command_count == before + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);
}

static void trace_legacy_marker_compatibility(void)
{
    screen_t *screen = started_screen(true);
    deauth_detector_data_t *data = screen->user_data;

    /* Older V2 builds can emit explicit lock/failure acknowledgements without
       the newer armed and confirming markers. They remain session status and
       never replace the independent detector start proofs. */
    rx("[DETECTOR_TRACE] lock_failed_channel=0 error=BAD");
    rx("[DETECTOR_TRACE] lock_failed_channel=9 error=");
    rx("[DETECTOR_TRACE] lock_failed_channel=9 error=BAD\tSTATE");
    rx("[DETECTOR_TRACE] lock_failed_channel=9 error=BAD\x80STATE");
    rx("[DETECTOR_TRACE] lock_failed_channel=9 error=12345678901234567890123456789012");
    assert(data->trace_status == TRACE_STATUS_WAITING && data->trace_channel == 0);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx(">[DETECTOR_TRACE] lock_failed_channel=9 error=ESP ERR INVALID STATE");
    assert(data->state == DET_START_ALL);
    assert(data->trace_status == TRACE_STATUS_TUNE_FAILED && data->trace_channel == 9);
    assert(strcmp(data->trace_error, "ESP ERR INVALID STATE") == 0);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("> [DETECTOR_TRACE] locked_channel=9");
    assert(data->state == DET_START_ALL);
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 9);
    assert(data->trace_error[0] == '\0');

    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_START_ALL);
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_ALL);
    screen->on_draw(screen);
    assert(ui_has("locked") && ui_has("CH 9"));

    key(screen, KEY_S);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK && data->trace_status == TRACE_STATUS_OFF);
    assert(data->trace_channel == 0 && data->trace_idle_ms == 0);
    close_screen(screen);
}

static void trace_idle_release_contract(void)
{
    screen_t *screen = started_screen(true);
    deauth_detector_data_t *data = screen->user_data;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    rx("[DETECTOR_INFO] format=deauth_v2");
    rx("[DETECTOR_TRACE] armed");
    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000");
    rx("[DEAUTH_V2] CH: 1 | BSSID: AA:BB:CC:DD:EE:FF | TX: AA:BB:CC:DD:EE:FF | DA: FF:FF:FF:FF:FF:FF | RSSI: -40 | REASON: 7 | SSID: Trace AP");
    rx("[DEAUTH_V2] CH: 2 | BSSID: AA:BB:CC:DD:EE:FF | TX: AA:BB:CC:DD:EE:FF | DA: FF:FF:FF:FF:FF:FF | RSSI: -39 | REASON: 7 | SSID: Trace AP");
    rx("[DETECTOR_TRACE] locked_channel=2");
    assert(data->state == DET_ACTIVE_ALL);
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 2);
    assert(data->deauth_count == 2 && data->csa_count == 0);

    /* A completed lock cannot be replaced by stale terminal markers. The
       same lock acknowledgement is harmless and idempotent. */
    rx("[DETECTOR_TRACE] locked_channel=2");
    rx("[DETECTOR_TRACE] locked_channel=11");
    rx("[DETECTOR_TRACE] lock_failed_channel=2 error=STALE_FAILURE");
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 2);
    assert(data->trace_error[0] == '\0');
    assert(data->deauth_count == 2 && data->csa_count == 0);

    /* Release syntax is exact, and a valid release for another channel is
       unsolicited. None of these lines can change events or the lock. */
    rx("[DETECTOR_TRACE] released_channel=0 reason=idle idle_ms=20000");
    rx("[DETECTOR_TRACE] released_channel=2 reason=busy idle_ms=20000");
    rx("[DETECTOR_TRACE] released_channel=2 reason=idle idle_ms=19999");
    rx("[DETECTOR_TRACE] released_channel=2 reason=idle idle_ms=20000 extra");
    rx("[DETECTOR_TRACE] released_channel=11 reason=idle idle_ms=20000");
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 2);
    assert(data->deauth_count == 2 && data->csa_count == 0);
    assert(strcmp(data->last_deauth.bssid, "AA:BB:CC:DD:EE:FF") == 0);

    /* Cardputer displays observations but never runs or refreshes JanOS's
       idle deadline. Only a later explicit release status changes Trace. */
    rx("[DEAUTH_V2] CH: 2 | BSSID: 02:00:00:00:00:99 | TX: 02:00:00:00:00:99 | DA: FF:FF:FF:FF:FF:FF | RSSI: -50 | REASON: 4 | SSID: Other AP");
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->deauth_count == 3);
    rx("[DEAUTH_V2] CH: 2 | BSSID: AA:BB:CC:DD:EE:FF | TX: AA:BB:CC:DD:EE:FF | DA: FF:FF:FF:FF:FF:FF | RSSI: -38 | REASON: 7 | SSID: Trace AP");
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->deauth_count == 4);
    assert(strcmp(data->last_deauth.bssid, "AA:BB:CC:DD:EE:FF") == 0);

    int saved_deauth = data->deauth_count;
    int saved_csa = data->csa_count;
    rx("> [DETECTOR_TRACE] released_channel=2 reason=idle idle_ms=20000");
    assert(data->trace_status == TRACE_STATUS_RELEASED);
    assert(data->trace_channel == 2 && data->trace_idle_ms == 20000);
    assert(data->deauth_count == saved_deauth && data->csa_count == saved_csa);
    assert(strcmp(data->last_deauth.bssid, "AA:BB:CC:DD:EE:FF") == 0);

    rx("[DETECTOR_TRACE] locked_channel=2");
    rx("[DETECTOR_TRACE] lock_failed_channel=2 error=STALE_FAILURE");
    assert(data->trace_status == TRACE_STATUS_RELEASED);
    assert(data->trace_channel == 2 && data->trace_idle_ms == 20000);

    rx(">[DETECTOR_TRACE] armed");
    assert(data->trace_status == TRACE_STATUS_RELEASED);
    assert(data->trace_channel == 2 && data->trace_idle_ms == 20000);
    assert(data->deauth_count == saved_deauth && data->csa_count == saved_csa);
    screen->on_draw(screen);
    assert(ui_has("released") && ui_has("CH2") && ui_has("rearmed"));

    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=6 window_ms=1000");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->trace_first_channel == 6 && data->trace_window_ms == 1000);
    rx("[DETECTOR_TRACE] locked_channel=6");
    rx("[DETECTOR_TRACE] released_channel=6 reason=idle idle_ms=20000");
    rx("[DETECTOR_TRACE] armed");
    assert(data->trace_status == TRACE_STATUS_RELEASED);
    assert(data->trace_channel == 6 && data->trace_idle_ms == 20000);

    key(screen, KEY_S);
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    screen->on_draw(screen);
    assert(ui_has("released") && ui_has("CH6") && ui_has("Waiting for JanOS stop ACK"));
    rx("Deauth Detector stopped.");
    assert(data->trace_status != TRACE_STATUS_OFF);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK && data->trace_status == TRACE_STATUS_OFF);
    assert(data->trace_channel == 0 && data->trace_idle_ms == 0);
    close_screen(screen);
}

static void trace_mode_contract(void)
{
    int before = command_count;
    int popped = pops;
    screen_t *screen = choice_screen();
    assert(command_count == before);
    screen->on_draw(screen);
    assert(ui_has("Trace") && ui_has("OFF"));
    key(screen, KEY_Q);
    assert(command_count == before && pops == popped + 1);
    close_screen(screen);

    screen = started_screen(true);
    deauth_detector_data_t *data = screen->user_data;
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 trace") == 0);
    assert(strstr(commands[command_count - 1], "--trace") == NULL);
    screen->on_draw(screen);
    assert(ui_has("Trace") && ui_has("wait arm"));
    before = command_count;
    rx("[DETECTOR_TRACE]Invalid detector option");
    assert(data->state == DET_START_ALL && command_count == before);
    assert(data->trace_status == TRACE_STATUS_WAITING);

    /* Real JanOS ordering: the independent start and format proofs make the
       watch active, then a prompt-prefixed CSA may arrive before the explicit
       Trace lock acknowledgement. CSA never changes the waiting Trace state. */
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_START_ALL);
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_ALL);
    assert(data->trace_status == TRACE_STATUS_WAITING);
    screen->on_draw(screen);
    assert(ui_has("wait arm") && !ui_has("locked"));
    rx("[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=1 window_ms=1000");
    rx("[DETECTOR_TRACE] armed extra");
    assert(data->trace_status == TRACE_STATUS_WAITING);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("> [DETECTOR_TRACE] armed");
    assert(data->trace_status != TRACE_STATUS_WAITING);
    screen->on_draw(screen);
    assert(ui_has("armed") && ui_has("DEAUTH"));
    rx("[DETECTOR_TRACE] confirming_bssid=not-a-mac first_channel=1 window_ms=1000");
    rx("[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=0 window_ms=1000");
    rx("[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=1 window_ms=999");
    rx("[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=1 window_ms=1000 extra");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    screen->on_draw(screen);
    assert(ui_has("armed") && ui_has("DEAUTH"));
    rx("> [CSA] RX: 2 | TO: 6 | BSSID: C2:18:03:96:35:01 | TX: C2:18:03:96:35:01 | RSSI: -35 | VIA: beacon | COUNT: 0 | MODE: 1 | CLASS: 81 | SSID: DIRECT-01-HP Laser 150nw");
    assert(data->deauth_count == 0 && data->csa_count == 1);
    assert(data->trace_status != TRACE_STATUS_WAITING);
    screen->on_draw(screen);
    assert(ui_has("armed") && ui_has("DEAUTH") && !ui_has("locked"));
    rx(">[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=1 window_ms=1000");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(strcmp(data->trace_bssid, "C2:18:03:96:35:01") == 0);
    assert(data->trace_first_channel == 1 && data->trace_window_ms == 1000);
    assert(data->deauth_count == 0 && data->csa_count == 1);
    screen->on_draw(screen);
    assert(ui_has("C2:18:03:96:35:01") && ui_has("CH1"));
    rx("> [CSA] RX: 2 | TO: 11 | BSSID: C2:18:03:96:35:01 | TX: C2:18:03:96:35:01 | RSSI: -36 | VIA: beacon | COUNT: 1 | MODE: 0 | CLASS: 81 | SSID: DIRECT-01-HP Laser 150nw");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->deauth_count == 0 && data->csa_count == 2);
    assert(data->last_csa.to == 11);

    /* A V2 observation never implies a lock. JanOS may emit several records
       during its one-second confirmation window. */
    rx("[DEAUTH_V2] CH: 1 | BSSID: C2:18:03:96:35:01 | TX: C2:18:03:96:35:01 | DA: FF:FF:FF:FF:FF:FF | RSSI: -34 | REASON: 7 | SSID: DIRECT-01-HP Laser 150nw");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->deauth_count == 1 && data->csa_count == 2);
    assert(data->last_deauth.reason_known && data->last_deauth.reason == 7);
    rx("[DEAUTH_V2] CH: 2 | BSSID: C2:18:03:96:35:01 | TX: C2:18:03:96:35:01 | DA: 02:00:00:00:00:05 | RSSI: -33 | REASON: - | SSID: DIRECT-01-HP Laser 150nw");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->deauth_count == 2 && data->csa_count == 2);
    assert(!data->last_deauth.reason_known);
    assert(strcmp(data->last_deauth.da, "02:00:00:00:00:05") == 0);
    screen->on_draw(screen);
    assert(ui_has("Reason -") && ui_has("Claimed TX"));
    assert(ui_has("DA 02:00:00:00:00:05"));

    rx("Output: [DETECTOR_TRACE] locked_channel=3");
    rx("[DETECTOR_TRACE] locked_channel=0");
    rx("[DETECTOR_TRACE] locked_channel=11 extra");
    rx("[DETECTOR_TRACE] confirming_bssid=C2:18:03:96:35:01 first_channel=256 window_ms=1000");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->deauth_count == 2 && data->csa_count == 2);
    assert(!data->last_deauth.reason_known);
    assert(strcmp(data->last_deauth.da, "02:00:00:00:00:05") == 0);
    assert(data->last_csa.to == 11);
    screen->on_draw(screen);
    assert(ui_has("C2:18:03:96:35:01") && ui_has("CH1") && !ui_has("locked"));
    rx("[DETECTOR_TRACE] locked_channel=2");
    assert(data->state == DET_ACTIVE_ALL);
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 2);
    assert(data->trace_bssid[0] == '\0');
    assert(data->trace_first_channel == 0 && data->trace_window_ms == 0);
    assert(data->deauth_count == 2 && data->csa_count == 2);
    assert(!data->last_deauth.reason_known);
    assert(strcmp(data->last_deauth.da, "02:00:00:00:00:05") == 0);
    screen->on_draw(screen);
    assert(ui_has("locked") && ui_has("CH 2"));
    screen->on_draw(screen);
    assert(ui_has("Trace") && ui_has("locked") && ui_has("CH 2"));

    /* The final stop acknowledgement clears the old lock. The session choice
       remains enabled for the selected-BSSID run and trace stays last. */
    key(screen, KEY_S);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    assert(data->trace_status == TRACE_STATUS_LOCKED);
    rx("Deauth Detector stopped.");
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(data->trace_status == TRACE_STATUS_LOCKED);
    rx("Status: All operations stopped.");
    rx("All operations stopped. trailing text");
    rx("[DEAUTH_V2] CH: 11 | BSSID: 84:A1:D1:C9:83:E0 | TX: 02:00:00:00:00:03 | DA: FF:FF:FF:FF:FF:FF | RSSI: -47 | REASON: 7 | SSID: All operations stopped.");
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(data->trace_status == TRACE_STATUS_LOCKED);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK);
    assert(data->trace_status == TRACE_STATUS_OFF);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1 trace") == 0);
    assert(data->trace_status == TRACE_STATUS_WAITING);
    screen->on_draw(screen);
    assert(ui_has("Trace") && ui_has("wait arm") && !ui_has("locked"));

    /* Trace acknowledgements remain independent of the start, format, and
       WATCH proofs. A failure can be followed by another confirmation and a
       successful lock without activating the selected watch by itself. */
    rx(">[DETECTOR_TRACE] armed");
    assert(data->state == DET_START_SELECTED);
    assert(data->trace_status == TRACE_STATUS_ARMED);
    rx("> [DETECTOR_TRACE] confirming_bssid=02:00:00:00:00:01 first_channel=6 window_ms=1000");
    assert(data->state == DET_START_SELECTED);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(strcmp(data->trace_bssid, "02:00:00:00:00:01") == 0);
    rx(">[DETECTOR_TRACE] lock_failed_channel=6 error=ESP_ERR_INVALID_STATE");
    assert(data->state == DET_START_SELECTED);
    assert(data->trace_status == TRACE_STATUS_TUNE_FAILED && data->trace_channel == 6);
    assert(data->trace_bssid[0] == '\0');
    assert(data->trace_first_channel == 0 && data->trace_window_ms == 0);
    assert(strcmp(data->trace_error, "ESP_ERR_INVALID_STATE") == 0);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    screen->on_draw(screen);
    assert(ui_has("Fail 6") && ui_has("hopping continues"));
    assert(ui_has("ESP_ERR_INVALID_STATE"));
    rx("> [DETECTOR_TRACE] confirming_bssid=02:00:00:00:00:01 first_channel=11 window_ms=1000");
    assert(data->state == DET_START_SELECTED);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(data->trace_first_channel == 11 && data->trace_window_ms == 1000);
    assert(data->trace_channel == 0 && data->trace_error[0] == '\0');
    rx(">[DETECTOR_TRACE] locked_channel=11");
    assert(data->state == DET_START_SELECTED);
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 11);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_START_SELECTED);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
    assert(data->state == DET_START_SELECTED);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home (upstairs)");
    assert(data->state == DET_ACTIVE_SELECTED);
    screen->on_draw(screen);
    assert(ui_has("locked") && ui_has("CH 11"));
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Home");
    assert(data->deauth_count == 0 && data->csa_count == 1);
    assert(data->trace_status == TRACE_STATUS_LOCKED && data->trace_channel == 11);
    screen->on_draw(screen);
    assert(ui_has("locked") && ui_has("CH 11"));
    close_screen(screen);

    /* Stop during confirmation keeps the remote Trace state visible until
       the exact final JanOS stop acknowledgement clears it. */
    screen = started_screen(true);
    data = screen->user_data;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    rx("[DETECTOR_INFO] format=deauth_v2");
    rx("[DETECTOR_TRACE] armed");
    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000");
    assert(data->state == DET_ACTIVE_ALL);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    key(screen, KEY_S);
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    assert(data->stop_pending);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(strcmp(data->trace_bssid, "AA:BB:CC:DD:EE:FF") == 0);
    screen->on_draw(screen);
    assert(ui_has("AA:BB:CC:DD:EE:FF") && ui_has("CH1"));
    assert(ui_has("Waiting for JanOS stop ACK"));
    rx("Deauth Detector stopped.");
    rx("All operations stopped. trailing text");
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(data->stop_pending);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    assert(strcmp(data->trace_bssid, "AA:BB:CC:DD:EE:FF") == 0);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK);
    assert(!data->stop_pending);
    assert(data->trace_status == TRACE_STATUS_OFF);
    assert(data->trace_bssid[0] == '\0');
    assert(data->trace_first_channel == 0 && data->trace_window_ms == 0);
    before = command_count;
    rx("> [DETECTOR_TRACE]Background scan started (bogus)");
    assert(data->state == DET_SCAN_WAIT_ACK && command_count == before);
    close_screen(screen);

    /* A safety Stop sent after a start timeout can be acknowledged after the
       screen enters its visible error state. Only that exact final reply
       clears the still-visible remote Trace state. */
    screen = started_screen(true);
    data = screen->user_data;
    rx("[DETECTOR_TRACE] armed");
    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000");
    tick(screen, 36000);
    assert(data->state == DET_ERROR);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    assert(data->stop_pending);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    screen->on_draw(screen);
    assert(ui_has("No JanOS start response"));
    assert(ui_has("AA:BB:CC:DD:EE:FF") && ui_has("CH1"));
    assert(ui_has("Waiting for JanOS stop ACK"));
    rx("Deauth Detector stopped.");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    rx("All operations stopped.");
    assert(data->state == DET_ERROR);
    assert(!data->stop_pending);
    assert(!data->exit_ready);
    assert(data->trace_status == TRACE_STATUS_OFF);
    assert(strstr(data->message, "No JanOS start response") != NULL);
    close_screen(screen);

    /* If an automatic safety Stop cannot be written, the UI must keep the
       last remote Trace state visible and say the detector may still run. */
    screen = started_screen(true);
    data = screen->user_data;
    rx("[DETECTOR_TRACE] armed");
    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000");
    before = command_count;
    send_result = 1;
    tick(screen, 36000);
    send_result = 0;
    assert(data->state == DET_ERROR && command_count == before);
    assert(!data->stop_pending && data->stop_send_failed);
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    screen->on_draw(screen);
    assert(ui_has("Stop send failed") && ui_has("may run"));
    assert(ui_has("AA:BB:CC:DD:EE:FF") && ui_has("CH1"));
    close_screen(screen);

    /* The same uncertainty is visible when a rejected, partly acknowledged
       start requires a safety Stop whose UART write fails. */
    screen = started_screen(true);
    data = screen->user_data;
    rx("[DETECTOR_TRACE] armed");
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->start_ack && data->state == DET_START_ALL);
    before = command_count;
    send_result = 1;
    rx("Regular sniffer is active");
    send_result = 0;
    assert(data->state == DET_ERROR && command_count == before);
    assert(!data->stop_pending && data->stop_send_failed);
    assert(data->trace_status == TRACE_STATUS_ARMED);
    screen->on_draw(screen);
    assert(ui_has("Stop send failed") && ui_has("may run"));
    assert(ui_has("Trace armed"));
    close_screen(screen);

    /* Older V2 can interpret the final token as an index. That explicit
       rejection is visible and never causes an automatic no-trace restart. */
    screen = started_screen(true);
    data = screen->user_data;
    before = command_count;
    rx("Invalid detector option; use --only before scan indices.");
    assert(data->state == DET_ERROR && command_count == before);
    screen->on_draw(screen);
    assert(ui_has("Trace unavailable"));
    close_screen(screen);

    screen = started_screen(true);
    data = screen->user_data;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    rx("[DETECTOR_INFO] format=deauth_v2");
    assert(data->state == DET_ACTIVE_ALL);
    key(screen, KEY_S);
    rx("All operations stopped.");
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1],
                  "deauth_detector_v2 --only 1 trace") == 0);
    before = command_count;
    rx("Invalid watch index 'trace' (valid: 1-2). Detector not started.");
    assert(data->state == DET_ERROR && command_count == before);
    screen->on_draw(screen);
    assert(ui_has("Trace unavailable"));
    close_screen(screen);

    /* Only the exact command-not-found reply retains the established legacy
       fallback. Trace remains visibly unavailable and is never appended. */
    screen = started_screen(true);
    data = screen->user_data;
    rx("[DETECTOR_TRACE] armed");
    rx("[DETECTOR_TRACE] confirming_bssid=AA:BB:CC:DD:EE:FF first_channel=1 window_ms=1000");
    assert(data->trace_status == TRACE_STATUS_CONFIRMING);
    rx("Unrecognized command");
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
    assert(strstr(commands[command_count - 1], "trace") == NULL);
    assert(data->trace_status == TRACE_STATUS_UNAVAILABLE);
    assert(data->trace_bssid[0] == '\0');
    assert(data->trace_first_channel == 0 && data->trace_window_ms == 0);
    screen->on_draw(screen);
    assert(ui_has("Trace unavailable"));
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    assert(data->state == DET_ACTIVE_ALL);
    screen->on_draw(screen);
    assert(ui_has("Trace unavailable"));
    key(screen, KEY_S);
    rx("All operations stopped.");
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1 trace") == 0);
    rx("Unrecognized command");
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1") == 0);
    assert(strstr(commands[command_count - 1], "trace") == NULL);
    screen->on_draw(screen);
    assert(ui_has("Trace unavailable"));
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home (upstairs)");
    assert(data->state == DET_ACTIVE_SELECTED);
    close_screen(screen);
}

int main(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    assert(screen && rx_callback);
    deauth_detector_data_t *data = screen->user_data;
    assert(full_clears == 1 && title_draws == 1);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    assert(data->state == DET_START_ALL);

    rx("Output: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>");
    rx("[DETECTOR_INFO] CSA/ECSA announcements enabled.");
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("[DEAUTH] CH: 6 | AP: Lab (upstairs) AP (02:Ab:00:00:00:01) | RSSI: -47");
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Lab (upstairs)");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    legacy_all_started(data);
    int clears_before = full_clears;
    int titles_before = title_draws;
    screen->on_draw(screen);
    assert(full_clears == clears_before && title_draws == titles_before);
    assert(content_clears >= 2);
    assert(strcmp(rows[1], "All networks / all channels") == 0);
    clears_before = full_clears;
    rx("> [DEAUTH] CH: 6 | AP: Lab (upstairs) AP (02:Ab:00:00:00:01) | RSSI: -47");
    tick(screen, 1);
    assert(full_clears == clears_before);
    assert(data->deauth_count == 1);
    assert(strcmp(data->last_deauth.ap_name, "Lab (upstairs) AP") == 0);
    assert(strcmp(data->last_deauth.bssid, "02:Ab:00:00:00:01") == 0);
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Lab (upstairs)");
    tick(screen, 1);
    assert(full_clears == clears_before);
    assert(data->csa_count == 1 && data->deauth_count == 1);
    assert(data->last_csa.rx == 6 && data->last_csa.to == 11);
    assert(strcmp(data->last_csa.ssid, "Lab (upstairs)") == 0);
    rx("[CSA] RX: 0 | TO: 0 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: action | COUNT: 0 | MODE: 255 | CLASS: 255 | SSID: <Unknown>");
    assert(data->csa_count == 2 && data->last_csa.rx == 0 && data->last_csa.to == 0);
    assert(data->last_csa.mode == 255 && strcmp(data->last_csa.class_name, "255") == 0);
    key(screen, KEY_C);
    clears_before = full_clears;
    titles_before = title_draws;
    screen->on_draw(screen);
    assert(full_clears == clears_before && title_draws == titles_before + 1);
    assert(strstr(rows[0], "CSA") && strstr(rows[4], "RX"));
    assert(strstr(rows[1], "unconfirmed"));
    assert(screen->on_resume);
    snprintf(rows[0], sizeof(rows[0]), "Other screen");
    clears_before = full_clears;
    screen->on_resume(screen);
    tick(screen, 1);
    assert(full_clears == clears_before + 1);
    assert(strcmp(rows[0], "CSA ANNOUNCEMENTS") == 0);

    enter_scan(screen);
    screen->on_draw(screen);
    assert(rows[1][0] == '\0' && rows[4][0] == '\0' && rows[6][0] == '\0');
    rx("\"1\",\"stale\",\"\",\"02:00:00:00:00:FF\",\"1\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->scan_count == 0 && data->state == DET_SCAN_WAIT_ACK);
    complete_scan_two(screen);
    screen->on_draw(screen);
    assert(strcmp(rows[1], "Selected networks 0/2") == 0);
    int before = command_count;
    key(screen, KEY_ENTER);
    assert(command_count == before && data->state == DET_PICK);
    tick(screen, 10000);
    assert(data->ping_waiting && strcmp(commands[command_count - 1], "ping") == 0);
    key(screen, KEY_SPACE);
    assert(data->scan[0].selected);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1") == 0);
    assert(data->state == DET_START_SELECTED);
    assert(!data->ping_waiting);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    assert(data->last_deauth.bssid[0] == '\0' && data->last_csa.bssid[0] == '\0');
    rx("[DEAUTH] CH: 6 | AP: Home (02:00:00:00:00:01) | RSSI: -50");
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:01 | RSSI: -40 | VIA: beacon | COUNT: 1 | MODE: 0 | CLASS: - | SSID: Home");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    legacy_selected_started(data);
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1") == 0);
    assert(data->state == DET_START_SELECTED);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home (upstairs)");
    assert(data->state == DET_ACTIVE_SELECTED && data->verified_watch_count == 1);
    screen->on_draw(screen);
    assert(strcmp(rows[1], "Selected BSSIDs: 1") == 0);
    tick(screen, 5000);
    assert(data->state == DET_ACTIVE_SELECTED);
    int selected_deauth_before = data->deauth_count;
    int selected_csa_before = data->csa_count;
    rx("[DEAUTH] CH: 6 | AP: Other (02:00:00:00:00:99) | RSSI: -42");
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:99 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Other");
    assert(data->deauth_count == selected_deauth_before && data->csa_count == selected_csa_before);
    rx("[DEAUTH] CH: 6 | AP: Home (upstairs) (02:00:00:00:00:01) | RSSI: -41");
    assert(data->deauth_count == selected_deauth_before + 1);
    key(screen, KEY_W);
    screen->on_draw(screen);
    assert(strstr(rows[1], "1") && strstr(rows[3], "02:00:00:00:00:01"));

    /* A second fresh scan clears the earlier selection; same SSID is not identity. */
    key(screen, KEY_S);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK && data->scan_count == 0);
    complete_scan_two(screen);
    assert(!data->scan[0].selected && !data->scan[1].selected);
    key(screen, KEY_SPACE);
    key(screen, KEY_DOWN);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1 2") == 0);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    legacy_selected_started(data);
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1 2") == 0);
    rx("[DETECTOR_WATCH] count=2 ap=1/2 CH=6 BSSID=02:00:00:00:00:01 SSID=Home");
    assert(data->state == DET_START_SELECTED);
    rx("[DETECTOR_WATCH] count=2 ap=2/2 CH=11 BSSID=02:00:00:00:00:02 SSID=Home");
    assert(data->state == DET_ACTIVE_SELECTED && data->verified_watch_count == 2);
    int popped = pops;
    int before_exit = command_count;
    key(screen, KEY_ESC);
    assert(pops == popped && command_count == before_exit);
    screen->on_draw(screen);
    assert(strcmp(rows[0], "EXIT DEAUTH DETECTOR?") == 0);
    assert(strstr(rows[7], "ENTER:Yes") != NULL);
    key(screen, KEY_Q); /* Cancel leaves the detector running. */
    assert(data->state == DET_ACTIVE_SELECTED && command_count == before_exit);
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT && command_count == before_exit + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("Deauth Detector stopped.");
    tick(screen, 1);
    assert(pops == popped);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    /* JanOS itself prints scan rows after WIFI_EVENT_SCAN_DONE, without a
       `Showing results ...` header. A stale `Scan still in progress` line
       cannot complete the list; only its marker can. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    int auto_before = command_count;
    tick(screen, 1000);
    assert(command_count == auto_before);
    rx("Scan still in progress... Please wait.");
    rx("WiFi scan completed. Found 2 networks, status: 0");
    rx("Retrieved 2 network records in 4.2s");
    rx("\"1\",\"JanOS A\",\"\",\"02:00:00:00:00:21\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("\"2\",\"JanOS B\",\"\",\"02:00:00:00:00:22\",\"11\",\"WPA2\",\"-50\",\"2.4GHz\"");
    assert(data->state != DET_PICK);
    rx("Scan results printed.");
    assert(data->state == DET_PICK && data->scan_count == 2);
    assert(data->scan[0].index == 1 && data->scan[1].index == 2);
    key(screen, KEY_DOWN);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 2") == 0);
    close_screen(screen);

    /* JanOS scan may repeat one BSSID; indices stay intact, WATCH is unique. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 2 network records in 3.1s");
    rx("\"1\",\"Mirror\",\"\",\"02:00:00:00:00:04\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("\"2\",\"Mirror\",\"\",\"02:00:00:00:00:04\",\"11\",\"WPA2\",\"-48\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->state == DET_PICK && data->scan_count == 2);
    key(screen, KEY_SPACE);
    key(screen, KEY_DOWN);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2 --only 1 2") == 0);
    assert(data->watch_expected_count == 1);
    legacy_selected_started(data);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:04 SSID=Mirror");
    assert(data->state == DET_ACTIVE_SELECTED);
    close_screen(screen);

    /* Incomplete rows and wrong 1-based index never become selectable. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 2 network records in 3.1s");
    rx("\"1\",\"One\",\"\",\"02:00:00:00:00:01\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 1 network records in 3.1s");
    rx("\"3\",\"Wrong\",\"\",\"02:00:00:00:00:03\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    /* No scan ACK, rejected start, no start ACK, and mismatched WATCH are visible. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    tick(screen, 6000);
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Scan failed with status: 1");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    /* JanOS omits the marker for zero results; a missing marker for a
       nonempty list must never make its indices selectable. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 0 network records in 2.1s");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 1 network records in 2.1s");
    rx("\"1\",\"No marker\",\"\",\"02:00:00:00:00:31\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    assert(data->state == DET_SCAN_COLLECT);
    tick(screen, 36000);
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    rx("Deauth detector already active. Use 'stop' to stop it first.");
    assert(data->state == DET_ERROR);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    tick(screen, 36000);
    assert(data->state == DET_ERROR);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    key(screen, KEY_R);
    assert(data->state == DET_STOP_FOR_SCAN);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("All operations stopped.");
    assert(data->state == DET_SCAN_WAIT_ACK);
    assert(strcmp(commands[command_count - 1], "scan_networks") == 0);
    close_screen(screen);

    /* Another scan on this UART invalidates a previously complete selection. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    legacy_selected_started(data);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:02 SSID=Wrong");
    assert(data->state != DET_ACTIVE_SELECTED);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    /* A matching BSSID with a different JanOS watch channel is unverified. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    legacy_selected_started(data);
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=11 BSSID=02:00:00:00:00:01 SSID=Home (upstairs)");
    assert(data->state != DET_ACTIVE_SELECTED);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    /* Selection is invalidated when the same UART stops answering pings. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    tick(screen, 11000);
    assert(strcmp(commands[command_count - 1], "ping") == 0);
    tick(screen, 6000);
    assert(data->state == DET_DISCONNECTED && data->scan_count == 0);
    close_screen(screen);

    /* Quoted SSIDs decode correctly, and A returns to the legacy command. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 1 network records in 3.1s");
    rx("\"1\",\"Lab \"\"A\"\" (1)\",\"\",\"02:00:00:00:00:01\",\"6\",\"WPA2\",\"-40\",\"2.4GHz\"");
    rx("Scan results printed.");
    assert(data->state == DET_PICK && strcmp(data->scan[0].ssid, "Lab \"A\" (1)") == 0);
    key(screen, KEY_A);
    assert(data->state == DET_START_ALL && data->scan_count == 0);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    key(screen, KEY_Q);
    assert(strcmp(commands[command_count - 1], "deauth_detector_v2") == 0);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("All operations stopped.");
    tick(screen, 1);
    close_screen(screen);

    /* JanOS must acknowledge --only and prove the requested BSSID. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    rx("Invalid watch index '1' (valid: 1-2). Detector not started.");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    key(screen, KEY_SPACE);
    key(screen, KEY_ENTER);
    tick(screen, 9000);
    assert(data->state == DET_ERROR);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    close_screen(screen);

    /* JanOS refuses a 65-AP watch scan even if the first rows fit locally. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    rx("Background scan started (min: 20 ms, max: 40 ms per channel)");
    rx("Retrieved 65 network records in 3.1s");
    assert(data->state == DET_ERROR && data->scan_count == 0);
    close_screen(screen);

    /* An old unanswered ping cannot invalidate the next mode. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    tick(screen, 11000);
    assert(strcmp(commands[command_count - 1], "ping") == 0);
    key(screen, KEY_A);
    assert(data->state == DET_START_ALL);
    legacy_all_started(data);
    tick(screen, 6000);
    assert(data->state == DET_ACTIVE_ALL);
    close_screen(screen);

    /* Failed stop send and absent stop ACK remain visible, with ESC retry. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    popped = pops;
    send_result = 1;
    key(screen, KEY_ESC);
    assert(pops == popped && data->state == DET_ACTIVE_ALL);
    key(screen, KEY_ENTER);
    assert(pops == popped && data->state == DET_ACTIVE_ALL);
    assert(strstr(data->message, "Stop") != NULL);
    send_result = 0;
    key(screen, KEY_Q);
    key(screen, KEY_ENTER);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    tick(screen, 36000);
    assert(pops == popped && data->state == DET_ERROR);
    assert(strstr(data->message, "stop response") != NULL);
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    rx("Deauth Detector stopped.");
    tick(screen, 1);
    assert(pops == popped && data->state == DET_STOP_EXIT);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    /* Even a completed scan or a lost connection must not bypass JanOS stop. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    enter_scan(screen);
    complete_scan_two(screen);
    popped = pops;
    before_exit = command_count;
    key(screen, KEY_ESC);
    assert(pops == popped && command_count == before_exit);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT && command_count == before_exit + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    tick(screen, 11000);
    tick(screen, 6000);
    assert(data->state == DET_DISCONNECTED);
    popped = pops;
    before_exit = command_count;
    key(screen, KEY_Q);
    assert(pops == popped && command_count == before_exit);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT && command_count == before_exit + 1);
    assert(strcmp(commands[command_count - 1], "stop") == 0);
    rx("Deauth Detector stopped.");
    tick(screen, 1);
    assert(pops == popped);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    /* Confirmed exit reuses an already sent stop without starting a scan. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    key(screen, KEY_S);
    assert(data->state == DET_STOP_FOR_SCAN);
    popped = pops;
    before_exit = command_count;
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT && command_count == before_exit);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1 && command_count == before_exit);
    close_screen(screen);

    /* A stop timeout while the exit question is open must reveal the error. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    key(screen, KEY_S);
    key(screen, KEY_ESC);
    screen->on_draw(screen);
    assert(strcmp(rows[0], "EXIT DEAUTH DETECTOR?") == 0);
    tick(screen, 36000);
    assert(data->state == DET_ERROR);
    screen->on_draw(screen);
    assert(strcmp(rows[0], "DEAUTH DETECTOR") == 0);
    assert(strstr(rows[2], "No JanOS stop response") != NULL);
    popped = pops;
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    /* Connection loss while confirming exit must replace the question. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    tick(screen, 11000);
    assert(data->ping_waiting);
    key(screen, KEY_ESC);
    tick(screen, 6000);
    assert(data->state == DET_DISCONNECTED);
    screen->on_draw(screen);
    assert(strcmp(rows[0], "DEAUTH DETECTOR") == 0);
    assert(strstr(rows[2], "JanOS connection lost") != NULL);
    popped = pops;
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    /* Confirming during an in-flight stop must keep its original deadline. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    legacy_all_started(data);
    key(screen, KEY_S);
    tick(screen, 34000);
    assert(data->state == DET_STOP_FOR_SCAN);
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT);
    tick(screen, 2000);
    assert(data->state == DET_ERROR);
    popped = pops;
    key(screen, KEY_ESC);
    key(screen, KEY_ENTER);
    assert(data->state == DET_STOP_EXIT);
    rx("All operations stopped.");
    tick(screen, 1);
    assert(pops == popped + 1);
    close_screen(screen);

    v2_all_records();
    prompt_prefixed_records();
    v2_selected_records();
    v2_start_timeout();
    trace_legacy_marker_compatibility();
    trace_idle_release_contract();
    trace_mode_contract();
    puts("ADV detector contract PASS");
    return 0;
}
