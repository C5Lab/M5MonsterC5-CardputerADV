/**
 * @file sta_mac_settings_screen.h
 * @brief JanOS STA MAC boot policy (NVS on C5, reboot to apply)
 */

#ifndef STA_MAC_SETTINGS_SCREEN_H
#define STA_MAC_SETTINGS_SCREEN_H

#include "screen_manager.h"

screen_t *sta_mac_settings_screen_create(void *params);

#endif /* STA_MAC_SETTINGS_SCREEN_H */
