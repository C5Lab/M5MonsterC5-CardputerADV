/**
 * @file bt_jammer_screen.h
 * @brief Bluetooth nRF24 jammer screen (band selector + START/STOP)
 */

#ifndef BT_JAMMER_SCREEN_H
#define BT_JAMMER_SCREEN_H

#include "screen_manager.h"

screen_t* bt_jammer_screen_create(void *params);

#endif
