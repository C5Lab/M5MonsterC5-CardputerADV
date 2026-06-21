/**
 * @file wardrive_config_screen.h
 * @brief Wardrive 2.0 setup screen (bands / channels / RSSI deltas / memcap /
 *        cooldown / anti-surv sensitivity / GPS), with Load and Apply.
 */

#ifndef WARDRIVE_CONFIG_SCREEN_H
#define WARDRIVE_CONFIG_SCREEN_H

#include "screen_manager.h"

screen_t* wardrive_config_screen_create(void *params);

#endif // WARDRIVE_CONFIG_SCREEN_H
