/**
 * @file wifi_connect_screen.h
 * @brief WiFi connection screen (SSID -> Password -> result)
 */

#ifndef WIFI_CONNECT_SCREEN_H
#define WIFI_CONNECT_SCREEN_H

#include "screen_manager.h"
#include <stdbool.h>

typedef struct {
    bool auto_close_on_success;
} wifi_connect_params_t;

/**
 * @brief Create the WiFi connect screen
 * @param params Optional wifi_connect_params_t*
 * @return Screen instance
 */
screen_t* wifi_connect_screen_create(void *params);

#endif // WIFI_CONNECT_SCREEN_H
