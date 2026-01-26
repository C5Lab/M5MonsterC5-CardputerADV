/**
 * @file rogue_ap_screen.h
 * @brief Rogue AP attack running screen
 */

#ifndef ROGUE_AP_SCREEN_H
#define ROGUE_AP_SCREEN_H

#include "screen_manager.h"

// Parameters for creating the Rogue AP screen
typedef struct {
    char ssid[33];
} rogue_ap_params_t;

/**
 * @brief Create the Rogue AP running screen
 * @param params Pointer to rogue_ap_params_t
 * @return Created screen or NULL on failure
 */
screen_t* rogue_ap_screen_create(void *params);

#endif // ROGUE_AP_SCREEN_H
