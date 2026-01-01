/**
 * @file airtag_scan_screen.h
 * @brief AirTag scan screen header
 */

#ifndef AIRTAG_SCAN_SCREEN_H
#define AIRTAG_SCAN_SCREEN_H

#include "screen_manager.h"

/**
 * @brief Create the AirTag scan screen
 * @param params Unused (pass NULL)
 * @return Pointer to the created screen, or NULL on failure
 */
screen_t* airtag_scan_screen_create(void *params);

#endif // AIRTAG_SCAN_SCREEN_H


