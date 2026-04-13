/**
 * @file nmap_screen.h
 * @brief Nmap port scanner screen
 */

#ifndef NMAP_SCREEN_H
#define NMAP_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the nmap scanner screen
 * @param params Not used
 * @return Screen instance
 */
screen_t* nmap_screen_create(void *params);

#endif // NMAP_SCREEN_H
