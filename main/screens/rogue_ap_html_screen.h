/**
 * @file rogue_ap_html_screen.h
 * @brief Rogue AP HTML selection screen
 */

#ifndef ROGUE_AP_HTML_SCREEN_H
#define ROGUE_AP_HTML_SCREEN_H

#include "screen_manager.h"

// Parameters for creating the Rogue AP HTML screen
typedef struct {
    char ssid[33];
    char password[65];
} rogue_ap_html_params_t;

/**
 * @brief Create the Rogue AP HTML selection screen
 * @param params Pointer to rogue_ap_html_params_t
 * @return Created screen or NULL on failure
 */
screen_t* rogue_ap_html_screen_create(void *params);

#endif // ROGUE_AP_HTML_SCREEN_H
