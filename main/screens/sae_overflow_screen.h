/**
 * @file sae_overflow_screen.h
 * @brief SAE Overflow attack running screen
 */

#ifndef SAE_OVERFLOW_SCREEN_H
#define SAE_OVERFLOW_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

// Parameters for creating the SAE overflow screen
typedef struct {
    wifi_network_t network;  // Single network being attacked (copied, not pointer)
} sae_overflow_screen_params_t;

/**
 * @brief Create the SAE overflow running screen
 * @param params Pointer to sae_overflow_screen_params_t
 * @return Created screen or NULL on failure
 */
screen_t* sae_overflow_screen_create(void *params);

#endif // SAE_OVERFLOW_SCREEN_H




