/**
 * @file wifi_connect_screen.c
 * @brief WiFi connection screen implementation
 * 
 * Flow: Choose method (Scan/Manual) -> SSID selection -> Password input
 *       -> send wifi_connect -> show result
 */

#include "wifi_connect_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI_CONNECT";

#define VISIBLE_ITEMS 5

typedef enum {
    STATE_CHOOSE_METHOD,
    STATE_SCANNING,
    STATE_PICK_NETWORK,
    STATE_ENTER_SSID,
    STATE_LOOKING_UP_PASS,
    STATE_ENTER_PASSWORD,
    STATE_CONNECTING,
    STATE_RESULT
} connect_state_t;

typedef struct {
    connect_state_t state;
    char ssid[33];
    char password[65];
    bool success;
    char result_msg[64];
    bool needs_redraw;
    bool needs_push_ssid_input;
    bool needs_push_password_input;
    bool needs_start_pass_lookup;
    int pass_timeout_ticks;
    bool pass_found;
    screen_t *self;

    int method_index;       // 0=Scan, 1=Manual
    wifi_network_t *networks;
    int network_count;
    int selected_index;
    int scroll_offset;
    int animation_frame;
    bool scan_complete;
    esp_timer_handle_t update_timer;
} wifi_connect_data_t;

// Forward declarations
static void draw_screen(screen_t *self);
static void on_ssid_submitted(const char *text, void *user_data);
static void on_password_submitted(const char *text, void *user_data);
static void start_scan(wifi_connect_data_t *data);
static void stop_scan_timer(wifi_connect_data_t *data);

/**
 * @brief UART callback for connection result
 */
static void uart_line_callback(const char *line, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    if (!data || data->state != STATE_CONNECTING) return;
    
    if (strstr(line, "SUCCESS:") != NULL && strstr(line, "Connected") != NULL) {
        data->success = true;
        snprintf(data->result_msg, sizeof(data->result_msg), "Connected to %s", data->ssid);
        data->state = STATE_RESULT;
        uart_set_wifi_connected(true);
        data->needs_redraw = true;
        ESP_LOGI(TAG, "WiFi connected successfully");
    }
    else if (strstr(line, "FAILED:") != NULL) {
        data->success = false;
        snprintf(data->result_msg, sizeof(data->result_msg), "Failed to connect");
        data->state = STATE_RESULT;
        uart_set_wifi_connected(false);
        data->needs_redraw = true;
        ESP_LOGW(TAG, "WiFi connection failed");
    }
}

static void start_connect(wifi_connect_data_t *data)
{
    data->state = STATE_CONNECTING;
    draw_screen(data->self);

    uart_register_line_callback(uart_line_callback, data);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wifi_connect %s %s", data->ssid, data->password);
    uart_send_command(cmd);
}

/**
 * @brief Parse a quoted field from a CSV-style line
 */
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

static bool is_pass_skip_line(const char *line)
{
    if (strlen(line) < 3) return true;
    if ((line[0] == 'I' || line[0] == 'W' || line[0] == 'E' || line[0] == 'D')
        && line[1] == ' ' && line[2] == '(') {
        return true;
    }
    if (strstr(line, "[MEM]") != NULL) return true;
    if (strncmp(line, "show_pass", 9) == 0) return true;
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '>') return true;
    return false;
}

/**
 * @brief UART callback for show_pass evil output
 */
static void pass_lookup_line_callback(const char *line, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    if (!data || data->state != STATE_LOOKING_UP_PASS) return;

    if (strlen(line) == 0) return;
    if (is_pass_skip_line(line)) return;

    if (strstr(line, "No ") != NULL || strstr(line, "no ") != NULL ||
        strstr(line, "empty") != NULL || strstr(line, "Empty") != NULL) {
        return;
    }

    char parsed_ssid[33] = {0};
    char parsed_pass[65] = {0};
    const char *p = line;

    if (!parse_pass_quoted_field(&p, parsed_ssid, sizeof(parsed_ssid))) return;
    if (!parse_pass_quoted_field(&p, parsed_pass, sizeof(parsed_pass))) return;

    if (strcasecmp(parsed_ssid, data->ssid) == 0) {
        strncpy(data->password, parsed_pass, sizeof(data->password) - 1);
        data->password[sizeof(data->password) - 1] = '\0';
        data->pass_found = true;
        ESP_LOGI(TAG, "Password found for %s", data->ssid);
    }
}

/**
 * @brief Scan complete callback from uart_handler
 */
static void on_scan_complete(wifi_network_t *networks, int count, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;

    ESP_LOGI(TAG, "Scan complete, %d networks", count);

    if (data->networks) {
        free(data->networks);
        data->networks = NULL;
    }

    if (count > 0) {
        data->networks = malloc(count * sizeof(wifi_network_t));
        if (data->networks) {
            memcpy(data->networks, networks, count * sizeof(wifi_network_t));
            data->network_count = count;
        }
    } else {
        data->network_count = 0;
    }

    data->scan_complete = true;
}

/**
 * @brief Timer callback for scan animation and completion check
 */
static void scan_timer_callback(void *arg)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)arg;

    if (data->scan_complete) {
        stop_scan_timer(data);

        if (data->network_count > 0) {
            data->state = STATE_PICK_NETWORK;
            data->selected_index = 0;
            data->scroll_offset = 0;
        } else {
            data->state = STATE_CHOOSE_METHOD;
        }
        data->needs_redraw = true;
        return;
    }

    data->animation_frame = (data->animation_frame + 1) % 4;

    int y3 = 3 * 16;
    display_fill_rect(0, y3, DISPLAY_WIDTH, 16, UI_COLOR_BG);
    const char *spinner[] = {"|", "/", "-", "\\"};
    char status[32];
    snprintf(status, sizeof(status), "Scanning... %s", spinner[data->animation_frame]);
    ui_print_center(3, status, UI_COLOR_TEXT);
}

static void stop_scan_timer(wifi_connect_data_t *data)
{
    if (data->update_timer) {
        esp_timer_stop(data->update_timer);
        esp_timer_delete(data->update_timer);
        data->update_timer = NULL;
    }
}

static void start_scan(wifi_connect_data_t *data)
{
    if (data->networks) {
        free(data->networks);
        data->networks = NULL;
    }
    data->network_count = 0;
    data->scan_complete = false;
    data->animation_frame = 0;
    data->state = STATE_SCANNING;

    draw_screen(data->self);

    esp_timer_create_args_t timer_args = {
        .callback = scan_timer_callback,
        .arg = data,
        .name = "wifi_conn_scan"
    };
    esp_timer_create(&timer_args, &data->update_timer);
    esp_timer_start_periodic(data->update_timer, 200000);

    esp_err_t ret = uart_start_wifi_scan(on_scan_complete, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan");
        stop_scan_timer(data);
        data->state = STATE_CHOOSE_METHOD;
        data->needs_redraw = true;
    }
}

static void push_password_input(wifi_connect_data_t *data)
{
    data->state = STATE_ENTER_PASSWORD;
    text_input_params_t *params = malloc(sizeof(text_input_params_t));
    if (params) {
        params->title = "Enter Password";
        params->hint = "WiFi password";
        params->on_submit = on_password_submitted;
        params->user_data = data;
        screen_manager_push(text_input_screen_create, params);
    }
}

/* ---- Drawing ---- */

static void draw_pick_row(wifi_connect_data_t *data, int net_idx)
{
    int row_on_screen = net_idx - data->scroll_offset;
    if (row_on_screen < 0 || row_on_screen >= VISIBLE_ITEMS) return;

    wifi_network_t *net = &data->networks[net_idx];
    char label[32];
    if (net->ssid[0]) {
        char ssid_short[20];
        strncpy(ssid_short, net->ssid, sizeof(ssid_short) - 1);
        ssid_short[sizeof(ssid_short) - 1] = '\0';
        snprintf(label, sizeof(label), "%.18s %ddB", ssid_short, net->rssi);
    } else {
        snprintf(label, sizeof(label), "[%.17s]", net->bssid);
    }
    bool is_selected = (net_idx == data->selected_index);
    ui_draw_menu_item(1 + row_on_screen, label, is_selected, false, false);
}

static void redraw_pick_two_rows(wifi_connect_data_t *data, int old_idx, int new_idx)
{
    draw_pick_row(data, old_idx);
    draw_pick_row(data, new_idx);
}

static void redraw_pick_list(wifi_connect_data_t *data)
{
    ui_draw_title("Pick Network");

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        int net_idx = data->scroll_offset + i;
        if (net_idx < data->network_count) {
            draw_pick_row(data, net_idx);
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
    if (data->scroll_offset + VISIBLE_ITEMS < data->network_count) {
        ui_print(UI_COLS - 2, VISIBLE_ITEMS, "v", UI_COLOR_DIMMED);
    }
}

static void draw_pick_network(wifi_connect_data_t *data)
{
    ui_clear();
    redraw_pick_list(data);
    ui_draw_status("ENTER:Select ESC:Back");
}

static void draw_screen(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    ui_clear();
    
    switch (data->state) {
        case STATE_CHOOSE_METHOD:
            ui_draw_title("WiFi Connect");
            ui_draw_menu_item(1, "Scan Networks", data->method_index == 0, false, false);
            ui_draw_menu_item(2, "Enter SSID", data->method_index == 1, false, false);
            ui_draw_status("UP/DOWN:Navigate ENTER:Select ESC:Back");
            break;

        case STATE_SCANNING:
            ui_draw_title("WiFi Scan");
            ui_print_center(3, "Scanning...", UI_COLOR_TEXT);
            ui_draw_status("ESC:Cancel");
            break;

        case STATE_PICK_NETWORK:
            draw_pick_network(data);
            break;

        case STATE_ENTER_SSID:
            ui_draw_title("WiFi Connect");
            ui_print_center(3, "Enter network SSID", UI_COLOR_TEXT);
            ui_draw_status("ESC:Back");
            break;
            
        case STATE_LOOKING_UP_PASS:
            ui_draw_title("WiFi Connect");
            ui_print_center(2, data->ssid, UI_COLOR_HIGHLIGHT);
            ui_print_center(4, "Looking up password...", UI_COLOR_DIMMED);
            ui_draw_status("ESC:Back");
            break;

        case STATE_ENTER_PASSWORD:
            ui_draw_title("WiFi Connect");
            ui_print_center(2, data->ssid, UI_COLOR_HIGHLIGHT);
            ui_print_center(4, "Enter password", UI_COLOR_TEXT);
            ui_draw_status("ESC:Back");
            break;
            
        case STATE_CONNECTING:
            ui_draw_title("WiFi Connect");
            ui_print_center(2, data->ssid, UI_COLOR_HIGHLIGHT);
            ui_print_center(4, "Connecting...", UI_COLOR_DIMMED);
            ui_draw_status("ESC:Back");
            break;
            
        case STATE_RESULT:
            ui_draw_title("WiFi Connect");
            if (data->success) {
                ui_print_center(3, data->result_msg, UI_COLOR_HIGHLIGHT);
            } else {
                ui_print_center(3, data->result_msg, UI_COLOR_TEXT);
            }
            ui_draw_status("ESC:Back");
            break;
    }
}

/* ---- Callbacks ---- */

static void on_ssid_submitted(const char *text, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    
    strncpy(data->ssid, text, sizeof(data->ssid) - 1);
    data->ssid[sizeof(data->ssid) - 1] = '\0';
    
    ESP_LOGI(TAG, "SSID entered: %s", data->ssid);
    
    screen_manager_pop();
    
    data->needs_start_pass_lookup = true;
}

static void on_password_submitted(const char *text, void *user_data)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)user_data;
    
    strncpy(data->password, text, sizeof(data->password) - 1);
    data->password[sizeof(data->password) - 1] = '\0';
    
    ESP_LOGI(TAG, "Password entered, connecting to %s", data->ssid);
    
    screen_manager_pop();
    start_connect(data);
}

/* ---- Tick / Key / Lifecycle ---- */

static void on_tick(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;
    
    if (data->needs_push_ssid_input) {
        data->needs_push_ssid_input = false;
        
        text_input_params_t *input_params = malloc(sizeof(text_input_params_t));
        if (input_params) {
            input_params->title = "Enter SSID";
            input_params->hint = "Network name";
            input_params->on_submit = on_ssid_submitted;
            input_params->user_data = data;
            screen_manager_push(text_input_screen_create, input_params);
        }
        return;
    }

    if (data->needs_start_pass_lookup) {
        data->needs_start_pass_lookup = false;
        data->pass_found = false;
        data->pass_timeout_ticks = 0;
        data->password[0] = '\0';
        data->state = STATE_LOOKING_UP_PASS;
        draw_screen(self);
        uart_register_line_callback(pass_lookup_line_callback, data);
        uart_send_command("show_pass evil");
        return;
    }

    if (data->needs_push_password_input) {
        data->needs_push_password_input = false;
        push_password_input(data);
        return;
    }

    if (data->state == STATE_LOOKING_UP_PASS) {
        if (data->pass_found) {
            uart_clear_line_callback();
            ESP_LOGI(TAG, "Auto-connecting to %s with found password", data->ssid);
            start_connect(data);
            return;
        }
        data->pass_timeout_ticks++;
        if (data->pass_timeout_ticks > 40) {
            uart_clear_line_callback();
            ESP_LOGI(TAG, "Password not found for %s, asking user", data->ssid);
            data->needs_push_password_input = true;
        }
        return;
    }
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;

    switch (data->state) {
        case STATE_CHOOSE_METHOD:
            switch (key) {
                case KEY_UP:
                    if (data->method_index > 0) {
                        int old = data->method_index;
                        data->method_index--;
                        ui_draw_menu_item(old + 1, old == 0 ? "Scan Networks" : "Enter SSID",
                                          false, false, false);
                        ui_draw_menu_item(data->method_index + 1,
                                          data->method_index == 0 ? "Scan Networks" : "Enter SSID",
                                          true, false, false);
                    } else {
                        int old = data->method_index;
                        data->method_index = 1;
                        ui_draw_menu_item(old + 1, "Scan Networks", false, false, false);
                        ui_draw_menu_item(2, "Enter SSID", true, false, false);
                    }
                    break;
                case KEY_DOWN:
                    if (data->method_index < 1) {
                        int old = data->method_index;
                        data->method_index++;
                        ui_draw_menu_item(old + 1,
                                          old == 0 ? "Scan Networks" : "Enter SSID",
                                          false, false, false);
                        ui_draw_menu_item(data->method_index + 1,
                                          data->method_index == 0 ? "Scan Networks" : "Enter SSID",
                                          true, false, false);
                    } else {
                        int old = data->method_index;
                        data->method_index = 0;
                        ui_draw_menu_item(old + 1, "Enter SSID", false, false, false);
                        ui_draw_menu_item(1, "Scan Networks", true, false, false);
                    }
                    break;
                case KEY_ENTER:
                case KEY_SPACE:
                    if (data->method_index == 0) {
                        start_scan(data);
                    } else {
                        data->state = STATE_ENTER_SSID;
                        data->needs_push_ssid_input = true;
                        draw_screen(self);
                    }
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                    screen_manager_pop();
                    break;
                default:
                    break;
            }
            break;

        case STATE_SCANNING:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                stop_scan_timer(data);
                data->state = STATE_CHOOSE_METHOD;
                draw_screen(self);
            }
            break;

        case STATE_PICK_NETWORK:
            switch (key) {
                case KEY_UP:
                    if (data->selected_index > 0) {
                        int old_idx = data->selected_index;
                        if (data->selected_index == data->scroll_offset && data->scroll_offset > 0) {
                            data->scroll_offset -= VISIBLE_ITEMS;
                            if (data->scroll_offset < 0) data->scroll_offset = 0;
                            data->selected_index = data->scroll_offset + VISIBLE_ITEMS - 1;
                            if (data->selected_index >= data->network_count)
                                data->selected_index = data->network_count - 1;
                            redraw_pick_list(data);
                        } else {
                            data->selected_index--;
                            redraw_pick_two_rows(data, old_idx, data->selected_index);
                        }
                    } else {
                        data->selected_index = data->network_count - 1;
                        data->scroll_offset = data->selected_index - VISIBLE_ITEMS + 1;
                        if (data->scroll_offset < 0) data->scroll_offset = 0;
                        redraw_pick_list(data);
                    }
                    break;
                case KEY_DOWN:
                    if (data->selected_index < data->network_count - 1) {
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
                    if (data->selected_index < data->network_count) {
                        wifi_network_t *net = &data->networks[data->selected_index];
                        strncpy(data->ssid, net->ssid, sizeof(data->ssid) - 1);
                        data->ssid[sizeof(data->ssid) - 1] = '\0';
                        ESP_LOGI(TAG, "Network selected: %s", data->ssid);
                        data->needs_start_pass_lookup = true;
                    }
                    break;
                case KEY_ESC:
                case KEY_BACKSPACE:
                    data->state = STATE_CHOOSE_METHOD;
                    draw_screen(self);
                    break;
                default:
                    break;
            }
            break;

        case STATE_LOOKING_UP_PASS:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                uart_clear_line_callback();
                data->state = STATE_CHOOSE_METHOD;
                draw_screen(self);
            }
            break;

        case STATE_ENTER_SSID:
        case STATE_ENTER_PASSWORD:
        case STATE_CONNECTING:
            if (key == KEY_ESC || key == KEY_BACKSPACE) {
                screen_manager_pop();
            }
            break;

        case STATE_RESULT:
            if (key == KEY_ENTER || key == KEY_SPACE ||
                key == KEY_ESC || key == KEY_BACKSPACE) {
                screen_manager_pop();
            }
            break;
    }
}

static void on_destroy(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;

    uart_clear_line_callback();

    if (data) {
        stop_scan_timer(data);
        if (data->networks) {
            free(data->networks);
        }
        free(data);
    }
}

static void on_resume(screen_t *self)
{
    wifi_connect_data_t *data = (wifi_connect_data_t *)self->user_data;

    if (data->state == STATE_ENTER_SSID && data->ssid[0] == '\0') {
        data->state = STATE_CHOOSE_METHOD;
    }

    draw_screen(self);
}

screen_t* wifi_connect_screen_create(void *params)
{
    (void)params;
    
    ESP_LOGI(TAG, "Creating WiFi connect screen...");
    
    screen_t *screen = screen_alloc();
    if (!screen) return NULL;
    
    wifi_connect_data_t *data = calloc(1, sizeof(wifi_connect_data_t));
    if (!data) {
        free(screen);
        return NULL;
    }
    
    data->state = STATE_CHOOSE_METHOD;
    data->self = screen;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "WiFi connect screen created");
    return screen;
}
