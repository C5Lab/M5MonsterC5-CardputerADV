/**
 * @file rogue_ap_ssid_screen.h
 * @brief Rogue AP SSID selection screen
 */

#ifndef ROGUE_AP_SSID_SCREEN_H
#define ROGUE_AP_SSID_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the Rogue AP SSID screen
typedef struct {
    wifi_network_t *networks;  // Selected networks (ownership transferred)
    int count;                 // Number of networks
} rogue_ap_ssid_params_t;

/**
 * @brief Create the Rogue AP SSID selection screen
 * @param params Pointer to rogue_ap_ssid_params_t
 * @return Created screen or NULL on failure
 */
screen_t* rogue_ap_ssid_screen_create(void *params);

#endif // ROGUE_AP_SSID_SCREEN_H
