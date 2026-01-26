/**
 * @file arp_attack_screen.c
 * @brief ARP attack screen implementation
 * 
 * Shows ARP poisoning in progress. ESC sends stop command and returns.
 */

#include "arp_attack_screen.h"
#include "uart_handler.h"
#include "text_ui.h"
#include "buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ARP_ATTACK";

// Screen state
typedef struct {
    char ip[16];
    char mac[18];
    char vendor[32];
    bool attack_active;
} arp_attack_data_t;

static void draw_screen(screen_t *self)
{
    arp_attack_data_t *data = (arp_attack_data_t *)self->user_data;
    
    ui_clear();
    ui_draw_title("ARP Poisoning");
    
    ui_print_center(1, "Target:", UI_COLOR_DIMMED);
    ui_print_center(2, data->ip, UI_COLOR_HIGHLIGHT);
    ui_print_center(3, data->mac, UI_COLOR_TEXT);
    ui_print_center(4, data->vendor, UI_COLOR_DIMMED);
    
    ui_draw_status("ESC:Stop Attack");
}

static void on_key(screen_t *self, key_code_t key)
{
    arp_attack_data_t *data = (arp_attack_data_t *)self->user_data;
    
    switch (key) {
        case KEY_ESC:
        case KEY_BACKSPACE:
            // Stop the attack
            if (data->attack_active) {
                ESP_LOGI(TAG, "Stopping ARP attack");
                uart_send_command("stop");
                data->attack_active = false;
            }
            screen_manager_pop();
            break;
            
        default:
            break;
    }
}

static void on_destroy(screen_t *self)
{
    arp_attack_data_t *data = (arp_attack_data_t *)self->user_data;
    
    // Make sure attack is stopped
    if (data && data->attack_active) {
        uart_send_command("stop");
    }
    
    if (self->user_data) {
        free(self->user_data);
    }
}

screen_t* arp_attack_screen_create(void *params)
{
    arp_attack_params_t *attack_params = (arp_attack_params_t *)params;
    
    ESP_LOGI(TAG, "Creating ARP attack screen for: %s (%s)", 
             attack_params ? attack_params->ip : "null",
             attack_params ? attack_params->mac : "null");
    
    screen_t *screen = screen_alloc();
    if (!screen) {
        if (attack_params) free(attack_params);
        return NULL;
    }
    
    // Allocate user data
    arp_attack_data_t *data = calloc(1, sizeof(arp_attack_data_t));
    if (!data) {
        free(screen);
        if (attack_params) free(attack_params);
        return NULL;
    }
    
    // Copy host info
    if (attack_params) {
        strncpy(data->ip, attack_params->ip, sizeof(data->ip) - 1);
        data->ip[sizeof(data->ip) - 1] = '\0';
        strncpy(data->mac, attack_params->mac, sizeof(data->mac) - 1);
        data->mac[sizeof(data->mac) - 1] = '\0';
        strncpy(data->vendor, attack_params->vendor, sizeof(data->vendor) - 1);
        data->vendor[sizeof(data->vendor) - 1] = '\0';
        free(attack_params);  // We own the passed params
    }
    
    data->attack_active = true;
    
    screen->user_data = data;
    screen->on_key = on_key;
    screen->on_destroy = on_destroy;
    screen->on_draw = draw_screen;
    
    // Draw screen
    draw_screen(screen);
    
    // Start the attack
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "arp_ban %s", data->mac);
    uart_send_command(cmd);
    buzzer_beep_attack();
    
    ESP_LOGI(TAG, "ARP attack started on %s (%s)", data->ip, data->mac);
    return screen;
}
