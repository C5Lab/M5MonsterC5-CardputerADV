/**
 * @file station_deauth_screen.h
 * @brief Station deauth attack running screen
 */

#ifndef STATION_DEAUTH_SCREEN_H
#define STATION_DEAUTH_SCREEN_H

#include "screen_manager.h"

// Parameters for creating the station deauth screen
typedef struct {
    char mac[18];   // MAC address of the station being attacked
    char ssid[33];  // SSID of the network the station is being deauthed from
} station_deauth_params_t;

/**
 * @brief Create the station deauth running screen
 * @param params Pointer to station_deauth_params_t (ownership transferred)
 * @return Created screen or NULL on failure
 */
screen_t* station_deauth_screen_create(void *params);

#endif // STATION_DEAUTH_SCREEN_H

