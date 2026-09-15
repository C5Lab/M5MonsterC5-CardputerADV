/**
 * @file cc1101_cap.h
 * @brief Slim CC1101 driver for the Cardputer Cap header (SPI3 + GDO0).
 *
 * Radio only: ASK/OOK RX edge capture and PATABLE TX. Decode/encode live on
 * Monster via subghz_ext_*.
 */

#ifndef CC1101_CAP_H
#define CC1101_CAP_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define CC1101_CAP_CS_PIN      6
#define CC1101_CAP_GDO0_PIN    4
#define CC1101_CAP_MOSI_PIN    14
#define CC1101_CAP_MISO_PIN    39
#define CC1101_CAP_SCK_PIN     40

#define CC1101_CAP_RAW_BUF_SIZE  4096
#define CC1101_CAP_DEFAULT_FREQ  433.92f

esp_err_t cc1101_cap_init(void);
bool cc1101_cap_is_initialized(void);

esp_err_t cc1101_cap_set_frequency(float mhz);
float cc1101_cap_get_frequency(void);

esp_err_t cc1101_cap_set_freq_correction(float mhz);
float cc1101_cap_get_freq_correction(void);

int cc1101_cap_get_rssi(void);

/** Apply OOK async RX preset and enter RX. */
void cc1101_cap_enter_rx(void);

void cc1101_cap_idle(void);

void cc1101_cap_capture_start(void);
void cc1101_cap_capture_stop(void);
int cc1101_cap_capture_count(void);
int64_t cc1101_cap_last_edge_us(void);
int cc1101_cap_copy_timings(int32_t *dst, int max);

esp_err_t cc1101_cap_tx_timings(float mhz, const int32_t *timings, int count);

/** Same as cc1101_cap_tx_timings, with an explicit repeat count (1..20). */
esp_err_t cc1101_cap_tx_timings_reps(float mhz, const int32_t *timings, int count,
                                     int reps);

esp_err_t cc1101_cap_jam_start(float mhz);
void cc1101_cap_jam_stop(void);
bool cc1101_cap_is_jamming(void);

#endif /* CC1101_CAP_H */
