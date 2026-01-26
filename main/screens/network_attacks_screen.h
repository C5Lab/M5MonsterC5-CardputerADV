/**
 * @file network_attacks_screen.h
 * @brief Network attacks menu screen (WiFi connect, ARP poisoning)
 */

#ifndef NETWORK_ATTACKS_SCREEN_H
#define NETWORK_ATTACKS_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the network attacks menu screen
 * @param params Not used
 * @return Screen instance
 */
screen_t* network_attacks_screen_create(void *params);

#endif // NETWORK_ATTACKS_SCREEN_H
