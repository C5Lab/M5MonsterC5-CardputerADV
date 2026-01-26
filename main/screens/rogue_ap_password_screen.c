/**
 * @file rogue_ap_password_screen.c
 * @brief Rogue AP password fetching/input screen
 * 
 * Sends "show_pass evil" to find password for SSID.
 * If not found, prompts user for password input.
 */

#include "rogue_ap_password_screen.h"
#include "rogue_ap_html_screen.h"
#include "text_input_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ROGUE_PASS";

#define MAX_ENTRIES 32

typedef struct {
    char ssid[33];
    char password[65];
} password_entry_t;

typedef enum {
    STATE_LOADING,
    STATE_FOUND,
    STATE_NOT_FOUND,
    STATE_WAITING_INPUT
} password_state_t;

typedef struct {
    char ssid[33];
    char password[65];
    password_entry_t entries[MAX_ENTRIES];
    int entry_count;
    password_state_t state;
    bool needs_redraw;
    int timeout_ticks;
    screen_t *self;
} rogue_ap_password_data_t;

static void draw_screen(screen_t *self);

/**
 * @brief Check if line is an ESP log line or command echo
 */
static bool is_skip_line(const char *line)
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
 * @brief Parse a quoted field from CSV
 */
static bool parse_quoted_field(const char **src, char *dest, size_t max_len)
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

/**
 * @brief UART callback for show_pass evil output
 */
static void uart_line_callback(const char *line, void *user_data)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)user_data;
    if (!data || data->entry_count >= MAX_ENTRIES) return;
    
    if (strlen(line) == 0) return;
    if (is_skip_line(line)) return;
    
    // Check for "No" or "empty" messages
    if (strstr(line, "No ") != NULL || strstr(line, "no ") != NULL || 
        strstr(line, "empty") != NULL || strstr(line, "Empty") != NULL) {
        if (data->state == STATE_LOADING) {
            data->state = STATE_NOT_FOUND;
            data->needs_redraw = true;
        }
        return;
    }
    
    // Parse: "SSID", "password"
    const char *p = line;
    password_entry_t *entry = &data->entries[data->entry_count];
    
    if (!parse_quoted_field(&p, entry->ssid, sizeof(entry->ssid))) return;
    if (!parse_quoted_field(&p, entry->password, sizeof(entry->password))) return;
    
    ESP_LOGI(TAG, "Parsed: SSID='%s', pass='%s'", entry->ssid, entry->password);
    data->entry_count++;
    
    // Check if this matches our target SSID
    if (strcasecmp(entry->ssid, data->ssid) == 0) {
        strncpy(data->password, entry->password, sizeof(data->password) - 1);
        data->state = STATE_FOUND;
        data->needs_redraw = true;
        ESP_LOGI(TAG, "Found password for %s: %s", data->ssid, data->password);
    }
}

static void proceed_to_html(rogue_ap_password_data_t *data)
{
    rogue_ap_html_params_t *params = malloc(sizeof(rogue_ap_html_params_t));
    if (params) {
        strncpy(params->ssid, data->ssid, sizeof(params->ssid) - 1);
        params->ssid[sizeof(params->ssid) - 1] = '\0';
        strncpy(params->password, data->password, sizeof(params->password) - 1);
        params->password[sizeof(params->password) - 1] = '\0';
        
        ESP_LOGI(TAG, "Proceeding to HTML select with SSID=%s, password=%s", 
                 params->ssid, params->password);
        screen_manager_push(rogue_ap_html_screen_create, params);
    }
}

/**
 * @brief Callback when user submits password
 */
static void on_password_submitted(const char *password, void *user_data)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)user_data;
    if (!data) return;
    
    if (password && strlen(password) > 0) {
        strncpy(data->password, password, sizeof(data->password) - 1);
        data->password[sizeof(data->password) - 1] = '\0';
        data->state = STATE_FOUND;
        
        // Pop text input and proceed to HTML
        screen_manager_pop();
        proceed_to_html(data);
    }
}

static void draw_screen(screen_t *self)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("Rogue AP Password");
    
    char ssid_line[40];
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %.24s", data->ssid);
    ui_print_center(2, ssid_line, UI_COLOR_TEXT);
    
    switch (data->state) {
        case STATE_LOADING:
            ui_print_center(4, "Searching for password...", UI_COLOR_DIMMED);
            break;
        case STATE_FOUND:
            ui_print_center(4, "Password found!", UI_COLOR_TEXT);
            ui_draw_status("ENTER:Continue ESC:Back");
            break;
        case STATE_NOT_FOUND:
            ui_print_center(4, "Password not found", UI_COLOR_DIMMED);
            ui_draw_status("ENTER:Enter password ESC:Back");
            break;
        case STATE_WAITING_INPUT:
            ui_print_center(4, "Enter password...", UI_COLOR_DIMMED);
            break;
    }
}

static void on_tick(screen_t *self)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)self->user_data;
    
    if (data->needs_redraw) {
        data->needs_redraw = false;
        draw_screen(self);
    }
    
    // Timeout after ~2 seconds if still loading
    if (data->state == STATE_LOADING) {
        data->timeout_ticks++;
        if (data->timeout_ticks > 40) {  // ~2 sec at 20 ticks/sec
            data->state = STATE_NOT_FOUND;
            data->needs_redraw = true;
        }
    }
}

static void on_key(screen_t *self, key_code_t key)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)self->user_data;
    
    switch (key) {
        case KEY_ENTER:
        case KEY_SPACE:
            if (data->state == STATE_FOUND) {
                proceed_to_html(data);
            } else if (data->state == STATE_NOT_FOUND) {
                // Open text input for password
                data->state = STATE_WAITING_INPUT;
                
                text_input_params_t *params = malloc(sizeof(text_input_params_t));
                if (params) {
                    params->title = "Enter Password";
                    params->hint = NULL;
                    params->on_submit = on_password_submitted;
                    params->user_data = data;
                    screen_manager_push(text_input_screen_create, params);
                }
            }
            break;
            
        case KEY_ESC:
        case KEY_Q:
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    uart_clear_line_callback();
    
    if (self->user_data) {
        free(self->user_data);
    }
}

static void on_resume(screen_t *self)
{
    rogue_ap_password_data_t *data = (rogue_ap_password_data_t *)self->user_data;
    
    // If we came back from text input, state might be WAITING_INPUT
    // but password was set - proceed to HTML
    if (data->state == STATE_WAITING_INPUT && data->password[0]) {
        proceed_to_html(data);
    } else {
        draw_screen(self);
    }
}

screen_t* rogue_ap_password_screen_create(void *params)
{
    rogue_ap_password_params_t *pass_params = (rogue_ap_password_params_t *)params;
    
    if (!pass_params) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Creating Rogue AP password screen for SSID: %s", pass_params->ssid);
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        free(pass_params);
        return NULL;
    }
    
    rogue_ap_password_data_t *data = calloc(1, sizeof(rogue_ap_password_data_t));
    if (!data) {
        free(screen);
        free(pass_params);
        return NULL;
    }
    
    strncpy(data->ssid, pass_params->ssid, sizeof(data->ssid) - 1);
    data->state = STATE_LOADING;
    data->self = screen;
    free(pass_params);
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_resume = on_resume;
    screen->on_draw = draw_screen;
    screen->on_tick = on_tick;
    
    // Register UART callback and send command
    uart_register_line_callback(uart_line_callback, data);
    uart_send_command("show_pass evil");
    
    draw_screen(screen);
    
    ESP_LOGI(TAG, "Rogue AP password screen created");
    return screen;
}
