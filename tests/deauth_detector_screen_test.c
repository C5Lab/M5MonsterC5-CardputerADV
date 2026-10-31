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
static char commands[128][256];
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
    assert(command_count < 128);
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
static void enter_scan(screen_t *screen)
{
    deauth_detector_data_t *data = screen->user_data;
    if (data->state == DET_START_ALL)
        rx("Deauth Detector started - scanning all channels for deauth frames...");
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

int main(void)
{
    screen_t *screen = deauth_detector_screen_create(NULL);
    assert(screen && rx_callback);
    deauth_detector_data_t *data = screen->user_data;
    assert(full_clears == 1 && title_draws == 1);
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
    assert(data->state == DET_START_ALL);

    rx("Output: [DEAUTH] CH: <ch> | AP: <name> (<bssid>) | RSSI: <rssi>");
    rx("[DETECTOR_INFO] CSA/ECSA announcements enabled.");
    rx("[DETECTOR_WATCH] count=1 ap=1/1 CH=6 BSSID=02:00:00:00:00:01 SSID=Home");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("[DEAUTH] CH: 6 | AP: Lab (upstairs) AP (02:Ab:00:00:00:01) | RSSI: -47");
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:03 | RSSI: -48 | VIA: beacon | COUNT: 3 | MODE: 0 | CLASS: - | SSID: Lab (upstairs)");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    int clears_before = full_clears;
    int titles_before = title_draws;
    screen->on_draw(screen);
    assert(full_clears == clears_before && title_draws == titles_before);
    assert(content_clears >= 2);
    assert(strcmp(rows[1], "All networks / all channels") == 0);
    clears_before = full_clears;
    rx("[DEAUTH] CH: 6 | AP: Lab (upstairs) AP (02:Ab:00:00:00:01) | RSSI: -47");
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
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1") == 0);
    assert(data->state == DET_START_SELECTED);
    assert(!data->ping_waiting);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    assert(data->last_deauth.bssid[0] == '\0' && data->last_csa.bssid[0] == '\0');
    rx("[DEAUTH] CH: 6 | AP: Home (02:00:00:00:00:01) | RSSI: -50");
    rx("[CSA] RX: 6 | TO: 11 | BSSID: 02:00:00:00:00:01 | TX: 02:00:00:00:00:01 | RSSI: -40 | VIA: beacon | COUNT: 1 | MODE: 0 | CLASS: - | SSID: Home");
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
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
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1 2") == 0);
    assert(data->deauth_count == 0 && data->csa_count == 0);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
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
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 2") == 0);
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
    assert(strcmp(commands[command_count - 1], "deauth_detector --only 1 2") == 0);
    assert(data->watch_expected_count == 1);
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
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
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
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
    rx("Deauth Detector started - monitoring selected channels for deauth frames...");
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
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
    key(screen, KEY_Q);
    assert(strcmp(commands[command_count - 1], "deauth_detector") == 0);
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
    tick(screen, 6000);
    assert(data->state == DET_ACTIVE_ALL);
    close_screen(screen);

    /* Failed stop send and absent stop ACK remain visible, with ESC retry. */
    screen = deauth_detector_screen_create(NULL);
    data = screen->user_data;
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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
    rx("Deauth Detector started - scanning all channels for deauth frames...");
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

    puts("ADV detector contract PASS");
    return 0;
}
