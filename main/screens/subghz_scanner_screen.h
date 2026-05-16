/**
 * @file subghz_scanner_screen.h
 * @brief Sub-GHz traffic scanner (MRU list of detected freqs)
 */

#ifndef SUBGHZ_SCANNER_SCREEN_H
#define SUBGHZ_SCANNER_SCREEN_H

#include "screen_manager.h"

screen_t* subghz_scanner_screen_create(void *params);

#endif
