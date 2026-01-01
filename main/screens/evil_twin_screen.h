/**
 * @file evil_twin_screen.h
 * @brief Evil Twin attack running screen
 */

#ifndef EVIL_TWIN_SCREEN_H
#define EVIL_TWIN_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the evil twin screen
typedef struct {
    wifi_network_t *networks;  // Networks being attacked (ownership transferred)
    int count;                 // Number of networks
} evil_twin_screen_params_t;

/**
 * @brief Create the evil twin running screen
 * @param params Pointer to evil_twin_screen_params_t
 * @return Created screen or NULL on failure
 */
screen_t* evil_twin_screen_create(void *params);

#endif // EVIL_TWIN_SCREEN_H




