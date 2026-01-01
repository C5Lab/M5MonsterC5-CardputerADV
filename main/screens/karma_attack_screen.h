/**
 * @file karma_attack_screen.h
 * @brief Karma attack running screen header
 */

#ifndef KARMA_ATTACK_SCREEN_H
#define KARMA_ATTACK_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Parameters for karma attack screen
 */
typedef struct {
    char ssid[33];   // SSID of the karma portal
} karma_attack_params_t;

/**
 * @brief Create the karma attack running screen
 * @param params Pointer to karma_attack_params_t
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* karma_attack_screen_create(void *params);

#endif // KARMA_ATTACK_SCREEN_H


