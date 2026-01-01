/**
 * @file global_portal_html_screen.h
 * @brief HTML selection screen for Global Portal attack
 */

#ifndef GLOBAL_PORTAL_HTML_SCREEN_H
#define GLOBAL_PORTAL_HTML_SCREEN_H

#include "screen_manager.h"

// Parameters for global portal HTML screen
typedef struct {
    char ssid[33];  // Portal SSID entered by user
} global_portal_html_params_t;

/**
 * @brief Create the global portal HTML selection screen
 * @param params global_portal_html_params_t pointer (takes ownership)
 * @return Screen instance
 */
screen_t* global_portal_html_screen_create(void *params);

#endif // GLOBAL_PORTAL_HTML_SCREEN_H


