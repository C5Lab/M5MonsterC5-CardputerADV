/**
 * @file pixie_dust_screen.h
 * @brief Pixie Dust WPS vulnerability check on one AP
 */

#ifndef PIXIE_DUST_SCREEN_H
#define PIXIE_DUST_SCREEN_H

#include "screen_manager.h"
#include "uart_handler.h"

typedef struct {
    wifi_network_t network;
} pixie_dust_screen_params_t;

screen_t *pixie_dust_screen_create(void *params);

#endif /* PIXIE_DUST_SCREEN_H */
