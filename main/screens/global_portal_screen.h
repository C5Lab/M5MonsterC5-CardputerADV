/**
 * @file global_portal_screen.h
 * @brief Global Portal running screen - monitors form submissions
 */

#ifndef GLOBAL_PORTAL_SCREEN_H
#define GLOBAL_PORTAL_SCREEN_H

#include "screen_manager.h"

// Parameters for global portal screen
typedef struct {
    char ssid[33];  // Portal SSID
} global_portal_params_t;

/**
 * @brief Create the global portal running screen
 * @param params global_portal_params_t pointer (takes ownership)
 * @return Screen instance
 */
screen_t* global_portal_screen_create(void *params);

#endif // GLOBAL_PORTAL_SCREEN_H


