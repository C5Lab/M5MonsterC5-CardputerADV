/**
 * @file subghz_settings_screen.h
 * @brief Global Sub-GHz Settings — CC1101 frequency correction.
 *
 * Reads the firmware-persisted xtal-trim correction via UART
 * (subghz_get_freq_correction / subghz_set_freq_correction). The value is
 * stored on the C5 NVS, not on the display.
 */

#ifndef SUBGHZ_SETTINGS_SCREEN_H
#define SUBGHZ_SETTINGS_SCREEN_H

#include "screen_manager.h"

screen_t* subghz_settings_screen_create(void *params);

#endif
