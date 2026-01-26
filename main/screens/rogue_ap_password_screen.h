/**
 * @file rogue_ap_password_screen.h
 * @brief Rogue AP password fetching/input screen
 */

#ifndef ROGUE_AP_PASSWORD_SCREEN_H
#define ROGUE_AP_PASSWORD_SCREEN_H

#include "screen_manager.h"

// Parameters for creating the Rogue AP password screen
typedef struct {
    char ssid[33];  // Selected SSID
} rogue_ap_password_params_t;

/**
 * @brief Create the Rogue AP password screen
 * @param params Pointer to rogue_ap_password_params_t
 * @return Created screen or NULL on failure
 */
screen_t* rogue_ap_password_screen_create(void *params);

#endif // ROGUE_AP_PASSWORD_SCREEN_H
