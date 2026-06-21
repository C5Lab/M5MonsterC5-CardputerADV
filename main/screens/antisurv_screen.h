/**
 * @file antisurv_screen.h
 * @brief Anti-Surveillance (BLE follower detection) screen.
 *
 * Mutually exclusive with wardrive (shared radio). Tracked via the
 * uart_is_antisurv_active / uart_set_antisurv_active flag.
 */

#ifndef ANTISURV_SCREEN_H
#define ANTISURV_SCREEN_H

#include "screen_manager.h"

screen_t* antisurv_screen_create(void *params);

#endif // ANTISURV_SCREEN_H
