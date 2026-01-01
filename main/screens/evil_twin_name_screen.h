/**
 * @file evil_twin_name_screen.h
 * @brief Evil Twin name selection screen
 */

#ifndef EVIL_TWIN_NAME_SCREEN_H
#define EVIL_TWIN_NAME_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the evil twin name screen
typedef struct {
    wifi_network_t *networks;  // Selected networks (ownership transferred)
    int count;                 // Number of networks
} evil_twin_name_params_t;

/**
 * @brief Create the evil twin name selection screen
 * @param params Pointer to evil_twin_name_params_t (ownership transferred)
 * @return Created screen or NULL on failure
 */
screen_t* evil_twin_name_screen_create(void *params);

#endif // EVIL_TWIN_NAME_SCREEN_H

