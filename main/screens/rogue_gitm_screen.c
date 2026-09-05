/**
 * @file rogue_gitm_screen.c
 * @brief Rogue GITM: scan → victim → same-ch uplink → connect → mirror → session
 */

#include "rogue_gitm_screen.h"
#include "gitm_session_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "ROGUE_GITM";

#define VISIBLE_ITEMS 5
#define CONNECT_TIMEOUT_TICKS 1500  /* wall-clock depends on main tick cadence */
/* screen_manager_tick runs ~every 500ms (main loop: 50 * 10ms) */
#define PASS_LOOKUP_TICKS     4     /* ~2s fallback if prompt never arrives */

typedef enum {
    STATE_SCANNING,
    STATE_PICK_VICTIM,
    STATE_PICK_UPLINK,
    STATE_LOOKUP_UPLINK_PASS,
    STATE_ENTER_UPLINK_PASS,
    STATE_CONNECTING,
    STATE_LOOKUP_MIRROR_PASS,
    STATE_ENTER_MIRROR_PASS,
    STATE_ERROR,
} rogue_state_t;

typedef struct {
    rogue_state_t state;
    wifi_network_t *networks;
    int count;
    int selected_index;
    int scroll_offset;

    wifi_network_t victim;
    wifi_network_t uplink;
    char uplink_pass[64];
    char mirror_pass[64];

    /* filtered uplink indices into networks[] */
    int uplink_map[MAX_NETWORKS];
    int uplink_count;

    bool pass_found;
    bool pass_lookup_done;  /* show_pass finished (CLI prompt '>') */
    bool show_pass_seen;    /* ignore leftover '>' from wifi_connect */
    int timeout_ticks;
    bool needs_redraw;
    bool needs_push_uplink_pass;
    bool needs_push_mirror_pass;
    bool needs_start_session;
    char error[80];
    screen_t *self;
} rogue_data_t;

static void draw_screen(screen_t *self);
static void build_uplink_list(rogue_data_t *data);
static bool network_is_open(const wifi_network_t *net);

static bool network_is_open(const wifi_network_t *net)
{
    if (!net) return false;
    if (net->security[0] == '\0') return true;
    return (strstr(net->security, "OPEN") != NULL);
}

static bool is_pass_skip_line(const char *line)
{
    if (!line || line[0] == '\0') return true;
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D')
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "show_pass", 9) == 0) return true;
    return false;
}

static bool line_is_cli_prompt(const char *line)
{
    const char *p = line;
    while (*p == ' ') p++;
    return (*p == '>');
}

static bool parse_pass_quoted_field(const char **src, char *dest, size_t max_len)
{
    const char *p = *src;
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        dest[i++] = *p++;
    }
    dest[i] = '\0';
    if (*p == '"') p++;
    *src = p;
    return true;
}

static void build_uplink_list(rogue_data_t *data)
{
    data->uplink_count = 0;
    for (int i = 0; i < data->count && data->uplink_count < MAX_NETWORKS; i++) {
        wifi_network_t *n = &data->networks[i];
        if (n->channel != data->victim.channel) continue;
        if (data->victim.bssid[0] && strcmp(n->bssid, data->victim.bssid) == 0)
            continue;
        data->uplink_map[data->uplink_count++] = i;
    }
}

static void on_scan_complete(wifi_network_t *networks, int count, void *user_data)
{
    rogue_data_t *data = (rogue_data_t *)user_data;
    if (!data) return;

    ESP_LOGI(TAG, "Scan complete, %d networks", count);

    if (data->networks) {
        free(data->networks);
        data->networks = NULL;
    }
    data->count = 0;

    if (count > 0) {
        data->networks = malloc(count * sizeof(wifi_network_t));
        if (data->networks) {
            memcpy(data->networks, networks, count * sizeof(wifi_network_t));
            data->count = count;
        }
    }

    data->selected_index = 0;
    data->scroll_offset = 0;
    data->state = STATE_PICK_VICTIM;
    data->needs_redraw = true;
}

static void pass_lookup_callback(const char *line, void *user_data)
{
    rogue_data_t *data = (rogue_data_t *)user_data;
    if (!data || !line) return;
    if (data->state != STATE_LOOKUP_UPLINK_PASS &&
        data->state != STATE_LOOKUP_MIRROR_PASS) {
        return;
    }

    /* Command echo — after this, the next bare '>' ends the list */
    if (strstr(line, "show_pass") != NULL) {
        data->show_pass_seen = true;
        return;
    }

    /* End of show_pass output — but ignore leftover prompt from wifi_connect */
    if (line_is_cli_prompt(line)) {
        if (data->show_pass_seen) {
            data->pass_lookup_done = true;
        }
        return;
    }

    if (is_pass_skip_line(line)) return;

    char parsed_ssid[33] = {0};
    char parsed_pass[65] = {0};
    const char *p = line;
    if (!parse_pass_quoted_field(&p, parsed_ssid, sizeof(parsed_ssid))) return;
    if (!parse_pass_quoted_field(&p, parsed_pass, sizeof(parsed_pass))) return;

    const char *target = (data->state == STATE_LOOKUP_UPLINK_PASS)
                       ? data->uplink.ssid : data->victim.ssid;
    if (strcasecmp(parsed_ssid, target) == 0) {
        if (data->state == STATE_LOOKUP_UPLINK_PASS) {
            strncpy(data->uplink_pass, parsed_pass, sizeof(data->uplink_pass) - 1);
        } else {
            strncpy(data->mirror_pass, parsed_pass, sizeof(data->mirror_pass) - 1);
        }
        data->pass_found = true;
    }
}

static void connect_line_callback(const char *line, void *user_data)
{
    rogue_data_t *data = (rogue_data_t *)user_data;
    if (!data || data->state != STATE_CONNECTING || !line) return;

    if (strstr(line, "SUCCESS") && strstr(line, "Connected")) {
        uart_set_wifi_connected(true);
        uart_clear_line_callback();
        data->pass_found = false;
        data->pass_lookup_done = false;
        data->show_pass_seen = false;
        data->timeout_ticks = 0;
        data->mirror_pass[0] = '\0';
        data->state = STATE_LOOKUP_MIRROR_PASS;
        data->needs_redraw = true;
        uart_register_line_callback(pass_lookup_callback, data);
        uart_send_command("show_pass evil");
        return;
    }
    if (strstr(line, "FAILED") || strstr(line, "TIMEOUT")) {
        uart_set_wifi_connected(false);
        uart_clear_line_callback();
        snprintf(data->error, sizeof(data->error), "Uplink connect failed");
        data->state = STATE_ERROR;
        data->needs_redraw = true;
    }
}

static void start_uplink_connect(rogue_data_t *data)
{
    data->state = STATE_CONNECTING;
    data->timeout_ticks = 0;
    data->needs_redraw = true;

    uart_register_line_callback(connect_line_callback, data);

    char cmd[160];
    if (data->uplink_pass[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "wifi_connect \"%s\" \"%s\"",
                 data->uplink.ssid, data->uplink_pass);
    } else {
        snprintf(cmd, sizeof(cmd), "wifi_connect \"%s\"", data->uplink.ssid);
    }
    ESP_LOGI(TAG, "Connecting uplink %s ch=%d", data->uplink.ssid, data->uplink.channel);
    uart_send_sensitive_command(cmd);
}

static void begin_uplink_pass_flow(rogue_data_t *data)
{
    data->uplink_pass[0] = '\0';
    if (network_is_open(&data->uplink)) {
        start_uplink_connect(data);
        return;
    }
    data->pass_found = false;
    data->pass_lookup_done = false;
    data->show_pass_seen = false;
    data->timeout_ticks = 0;
    data->state = STATE_LOOKUP_UPLINK_PASS;
    data->needs_redraw = true;
    uart_register_line_callback(pass_lookup_callback, data);
    uart_send_command("show_pass evil");
}

static void on_uplink_pass_submitted(const char *text, void *user_data)
{
    rogue_data_t *data = (rogue_data_t *)user_data;
    if (!data) return;
    if (text) {
        strncpy(data->uplink_pass, text, sizeof(data->uplink_pass) - 1);
        data->uplink_pass[sizeof(data->uplink_pass) - 1] = '\0';
    }
    screen_manager_pop();
    /* Connect after pop via tick — set flag by starting connect when resumed.
     * Parent is still this screen under the text input; after pop on_resume runs.
     * Use a deferred connect: set state then call from on_tick. */
    data->state = STATE_CONNECTING;
    data->timeout_ticks = 0;
    data->needs_redraw = true;
    /* Mark that connect should start on next tick */
    data->needs_push_uplink_pass = false;
    /* Reuse needs_start_session briefly? Better: dedicated by calling start in on_resume/tick.
     * Store intent: timeout_ticks = -1 means "start connect now" */
    data->timeout_ticks = -1;
}

static void on_mirror_pass_submitted(const char *text, void *user_data)
{
    rogue_data_t *data = (rogue_data_t *)user_data;
    if (!data || !text) return;

    size_t len = strlen(text);
    if (len < 8 || len > 63) {
        data->needs_push_mirror_pass = true;
        screen_manager_pop();
        return;
    }

    strncpy(data->mirror_pass, text, sizeof(data->mirror_pass) - 1);
    data->mirror_pass[sizeof(data->mirror_pass) - 1] = '\0';
    data->needs_start_session = true;
    screen_manager_pop();
}

static void push_uplink_pass(rogue_data_t *data)
{
    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "Uplink Password";
    p->hint = "Home WiFi password";
    p->on_submit = on_uplink_pass_submitted;
    p->user_data = data;
    p->allow_empty = false;
    p->max_length = 63;
    p->masked = true;
    data->state = STATE_ENTER_UPLINK_PASS;
    screen_manager_push(text_input_screen_create, p);
}

static void push_mirror_pass(rogue_data_t *data)
{
    text_input_params_t *p = calloc(1, sizeof(text_input_params_t));
    if (!p) return;
    p->title = "Mirror Password";
    p->hint = "Victim WPA2 (8-63)";
    p->on_submit = on_mirror_pass_submitted;
    p->user_data = data;
    p->allow_empty = false;
    p->max_length = 63;
    p->masked = true;
    data->state = STATE_ENTER_MIRROR_PASS;
    screen_manager_push(text_input_screen_create, p);
}

static void start_session(rogue_data_t *data)
{
    gitm_session_params_t *sp = calloc(1, sizeof(gitm_session_params_t));
    if (!sp) return;
    sp->mode = GITM_MODE_ROGUE;
    strncpy(sp->ssid, data->victim.ssid, sizeof(sp->ssid) - 1);
    strncpy(sp->password, data->mirror_pass, sizeof(sp->password) - 1);
    sp->victim_index = data->victim.id;
    screen_manager_replace(gitm_session_screen_create, sp);
}

static bool pick_is_uplink(const rogue_data_t *data)
{
    return data->state == STATE_PICK_UPLINK;
}

static int pick_list_count(const rogue_data_t *data)
{
    return pick_is_uplink(data) ? data->uplink_count : data->count;
}

static void pick_title(const rogue_data_t *data, char *out, size_t out_sz)
{
    if (pick_is_uplink(data)) {
        snprintf(out, out_sz, "Uplink ch%d", data->victim.channel);
    } else {
        snprintf(out, out_sz, "Pick Victim");
    }
}

static void draw_pick_row(rogue_data_t *data, int list_idx)
{
    int row_on_screen = list_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;

    int list_count = pick_list_count(data);
    if (list_idx < 0 || list_idx >= list_count) return;

    wifi_network_t *net;
    if (pick_is_uplink(data)) {
        net = &data->networks[data->uplink_map[list_idx]];
    } else {
        net = &data->networks[list_idx];
    }

    char label[30];
    if (net->ssid[0]) {
        snprintf(label, sizeof(label), "%.20s ch%d", net->ssid, net->channel);
    } else {
        snprintf(label, sizeof(label), "[hidden] ch%d", net->channel);
    }
    ui_draw_menu_item(1 + row_on_screen, label,
                      list_idx == data->selected_index, false, false);
}

static void redraw_pick_two_rows(rogue_data_t *data, int old_idx, int new_idx)
{
    draw_pick_row(data, old_idx);
    draw_pick_row(data, new_idx);
}

static void redraw_pick_list(rogue_data_t *data)
{
    char title[24];
    pick_title(data, title, sizeof(title));
    ui_draw_title(title);

    int list_count = pick_list_count(data);
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int idx = data->scroll_offset + i;
        if (idx < list_count) {
            draw_pick_row(data, idx);
        } else {
            int y = (1 + i) * 16;
            display_fill_rect(0, y, DISPLAY_WIDTH, 16, UI_COLOR_BG);
        }
    }

    display_fill_rect(DISPLAY_WIDTH - 16, 1 * 16, 16, 16, UI_COLOR_BG);
    display_fill_rect(DISPLAY_WIDTH - 16, VISIBLE_ITEMS * 16, 16, 16, UI_COLOR_BG);
    if (data->scroll_offset > 0) {
        ui_print(UI_COLS - 2, 1, "^", UI_COLOR_DIMMED);
    }
    if (data->scroll_offset + VISIBLE_ITEMS < list_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static void draw_pick_list(rogue_data_t *data)
{
    ui_clear();

    if (pick_is_uplink(data) && data->uplink_count == 0) {
        char title[24];
        pick_title(data, title, sizeof(title));
        ui_draw_title(title);
        char msg[36];
        snprintf(msg, sizeof(msg), "No other AP on ch%d", data->victim.channel);
        ui_print_center(3, msg, UI_COLOR_HIGHLIGHT);
        ui_draw_status("ESC:Back");
        return;
    }

    redraw_pick_list(data);
    ui_draw_status("ENTER:Select ESC:Back");
}

static void draw_screen(screen_t *self)
{
    rogue_data_t *data = (rogue_data_t *)self->user_data;
    if (!data) return;

    switch (data->state) {
        case STATE_SCANNING:
            ui_clear();
            ui_draw_title("Rogue GITM");
            ui_print_center(3, "Scanning...", UI_COLOR_HIGHLIGHT);
            ui_draw_status("ESC:Cancel");
            break;
        case STATE_PICK_VICTIM:
        case STATE_PICK_UPLINK:
            draw_pick_list(data);
            break;
        case STATE_LOOKUP_UPLINK_PASS:
        case STATE_LOOKUP_MIRROR_PASS:
            ui_clear();
            ui_draw_title("Rogue GITM");
            ui_print_center(3, "Looking up password...", UI_COLOR_HIGHLIGHT);
            ui_draw_status("Please wait");
            break;
        case STATE_ENTER_UPLINK_PASS:
        case STATE_ENTER_MIRROR_PASS:
            /* text_input is on top */
            break;
        case STATE_CONNECTING:
            ui_clear();
            ui_draw_title("Rogue GITM");
            {
                char buf[40];
                snprintf(buf, sizeof(buf), "Connecting %.20s...", data->uplink.ssid);
                ui_print_center(3, buf, UI_COLOR_HIGHLIGHT);
            }
            ui_draw_status("ESC:Cancel");
            break;
        case STATE_ERROR:
            ui_clear();
            ui_draw_title("Rogue GITM");
            ui_print_center(3, data->error, UI_COLOR_HIGHLIGHT);
            ui_draw_status("ESC:Back");
            break;
    }
}

static void on_select_victim(rogue_data_t *data)
{
    if (data->count == 0) return;
    wifi_network_t *net = &data->networks[data->selected_index];
    if (!net->ssid[0]) {
        snprintf(data->error, sizeof(data->error), "Hidden SSID unsupported");
        data->state = STATE_ERROR;
        data->needs_redraw = true;
        return;
    }
    data->victim = *net;
    build_uplink_list(data);
    data->selected_index = 0;
    data->scroll_offset = 0;
    data->state = STATE_PICK_UPLINK;
    data->needs_redraw = true;
}

static void on_select_uplink(rogue_data_t *data)
{
    if (data->uplink_count == 0) return;
    int net_idx = data->uplink_map[data->selected_index];
    data->uplink = data->networks[net_idx];
    begin_uplink_pass_flow(data);
}

static void on_key(screen_t *self, key_code_t key)
{
    rogue_data_t *data = (rogue_data_t *)self->user_data;
    if (!data) return;

    if (data->state == STATE_ERROR) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            /* Recoverable errors return to victim pick; fatal scan failure exits */
            if (data->networks && data->count > 0) {
                data->error[0] = '\0';
                data->state = STATE_PICK_VICTIM;
                data->selected_index = 0;
                data->scroll_offset = 0;
                data->needs_redraw = true;
            } else {
                screen_manager_pop();
            }
        }
        return;
    }

    if (data->state == STATE_SCANNING || data->state == STATE_CONNECTING ||
        data->state == STATE_LOOKUP_UPLINK_PASS ||
        data->state == STATE_LOOKUP_MIRROR_PASS) {
        if (key == KEY_ESC || key == KEY_Q || key == KEY_BACKSPACE) {
            if (data->state == STATE_CONNECTING ||
                data->state == STATE_LOOKUP_UPLINK_PASS ||
                data->state == STATE_LOOKUP_MIRROR_PASS) {
                uart_clear_line_callback();
            }
            screen_manager_pop();
        }
        return;
    }

    if (data->state != STATE_PICK_VICTIM && data->state != STATE_PICK_UPLINK) {
        return;
    }

    int list_count = pick_list_count(data);

    switch (key) {
        case KEY_UP:
            if (list_count <= 0) break;
            if (data->selected_index > 0) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                    data->scroll_offset -= VISIBLE_ITEMS;
                    if (data->scroll_offset < 0) data->scroll_offset = 0;
                    data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                    if (data->selected_index >= list_count)
                        data->selected_index = list_count - 1;
                    redraw_pick_list(data);
                } else {
                    data->selected_index--;
                    redraw_pick_two_rows(data, old_idx, data->selected_index);
                }
            } else {
                data->selected_index = list_count - 1;
                data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                if (data->scroll_offset < 0) data->scroll_offset = 0;
                redraw_pick_list(data);
            }
            break;

        case KEY_DOWN:
            if (list_count <= 0) break;
            if (data->selected_index < list_count - 1) {
                int old_idx = data->selected_index;
                if (data->selected_index == data->scroll_offset + VISIBLE_ITEMS - 1) {
                    data->scroll_offset += VISIBLE_ITEMS;
                    data->selected_index = data->scroll_offset;
                    redraw_pick_list(data);
                } else {
                    data->selected_index++;
                    redraw_pick_two_rows(data, old_idx, data->selected_index);
                }
            } else {
                data->selected_index = 0;
                data->scroll_offset = 0;
                redraw_pick_list(data);
            }
            break;

        case KEY_ENTER:
        case KEY_SPACE:
            if (data->state == STATE_PICK_VICTIM) {
                on_select_victim(data);
            } else if (data->state == STATE_PICK_UPLINK) {
                on_select_uplink(data);
            }
            break;

        case KEY_ESC:
        case KEY_Q:
        case KEY_BACKSPACE:
            if (data->state == STATE_PICK_UPLINK) {
                data->state = STATE_PICK_VICTIM;
                data->selected_index = 0;
                data->scroll_offset = 0;
                data->needs_redraw = true;
            } else {
                screen_manager_pop();
            }
            break;

        default:
            break;
    }
}

static void on_tick(screen_t *self)
{
    rogue_data_t *data = (rogue_data_t *)self->user_data;
    if (!data) return;

    if (data->needs_push_uplink_pass) {
        data->needs_push_uplink_pass = false;
        push_uplink_pass(data);
        return;
    }

    if (data->needs_push_mirror_pass) {
        data->needs_push_mirror_pass = false;
        push_mirror_pass(data);
        return;
    }

    if (data->needs_start_session) {
        data->needs_start_session = false;
        start_session(data);
        return;
    }

    /* Deferred connect after uplink password input */
    if (data->state == STATE_CONNECTING && data->timeout_ticks < 0) {
        data->timeout_ticks = 0;
        start_uplink_connect(data);
        return;
    }

    if (data->state == STATE_LOOKUP_UPLINK_PASS) {
        data->timeout_ticks++;
        if (data->pass_found) {
            uart_clear_line_callback();
            start_uplink_connect(data);
            return;
        }
        if (data->pass_lookup_done || data->timeout_ticks >= PASS_LOOKUP_TICKS) {
            uart_clear_line_callback();
            data->needs_push_uplink_pass = true;
            return;
        }
    }

    if (data->state == STATE_LOOKUP_MIRROR_PASS) {
        data->timeout_ticks++;
        if (data->pass_found && strlen(data->mirror_pass) >= 8) {
            uart_clear_line_callback();
            data->needs_start_session = true;
            return;
        }
        if (data->pass_lookup_done || data->timeout_ticks >= PASS_LOOKUP_TICKS) {
            uart_clear_line_callback();
            data->needs_push_mirror_pass = true;
            return;
        }
    }

    if (data->state == STATE_CONNECTING && data->timeout_ticks >= 0) {
        data->timeout_ticks++;
        if (data->timeout_ticks >= CONNECT_TIMEOUT_TICKS) {
            uart_clear_line_callback();
            snprintf(data->error, sizeof(data->error), "Connect timeout");
            data->state = STATE_ERROR;
            data->needs_redraw = true;
        }
    }

    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_resume(screen_t *self)
{
    draw_screen(self);
}

static void on_destroy(screen_t *self)
{
    rogue_data_t *data = (rogue_data_t *)self->user_data;
    uart_clear_line_callback();
    if (data) {
        if (data->networks) free(data->networks);
        free(data);
    }
}

screen_t *rogue_gitm_screen_create(void *params)
{
    (void)params;

    ESP_LOGI(TAG, "Creating Rogue GITM wizard");

    screen_t *screen = screen_alloc();
    if (!screen) return NULL;

    rogue_data_t *data = calloc(1, sizeof(rogue_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }

    data->self = screen;
    data->state = STATE_SCANNING;

    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;

    draw_screen(screen);

    esp_err_t ret = uart_start_wifi_scan(on_scan_complete, data);
    if (ret != ESP_OK) {
        snprintf(data->error, sizeof(data->error), "Scan failed");
        data->state = STATE_ERROR;
        draw_screen(screen);
    }

    return screen;
}
