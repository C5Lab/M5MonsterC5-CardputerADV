/**
 * @file subghz_tesla_screen.h
 * @brief Tesla charge-port opener (single-shot 315 MHz transmit)
 */

#ifndef SUBGHZ_TESLA_SCREEN_H
#define SUBGHZ_TESLA_SCREEN_H

#include "screen_manager.h"

screen_t* subghz_tesla_screen_create(void *params);

#endif
