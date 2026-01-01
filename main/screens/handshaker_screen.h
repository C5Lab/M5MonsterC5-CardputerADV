/**
 * @file handshaker_screen.h
 * @brief Handshaker attack running screen
 */

#ifndef HANDSHAKER_SCREEN_H
#define HANDSHAKER_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the handshaker screen
typedef struct {
    wifi_network_t *networks;  // Networks being attacked (ownership transferred)
    int count;                 // Number of networks
} handshaker_screen_params_t;

/**
 * @brief Create the handshaker running screen
 * @param params Pointer to handshaker_screen_params_t
 * @return Created screen or NULL on failure
 */
screen_t* handshaker_screen_create(void *params);

#endif // HANDSHAKER_SCREEN_H



