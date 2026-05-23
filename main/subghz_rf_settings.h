/**
 * @file subghz_rf_settings.h
 * @brief Persistent SubGHz RF settings (Hunter / Scanner / Listen).
 *
 * NVS namespace: "subghz_rf". Values are clamped on load and save. The CLI
 * command builders below produce the exact same UART payloads as the coreS3
 * reference UI so the JanOS firmware sees an identical command surface.
 *
 * Note: the firmware-side CC1101 frequency correction (subghz_set/get_freq_
 * correction) is persisted on the C5 itself and is NOT in this struct.
 */

#ifndef SUBGHZ_RF_SETTINGS_H
#define SUBGHZ_RF_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int8_t   hunter_trigger_dbm;   /* -120..-40 */
    uint16_t hunter_timeout_ms;    /* 100..60000 */
    bool     hunter_raw;
    bool     hunter_single;
    bool     hunter_fast;
    uint16_t scanner_dwell_ms;     /* 20..500 */
    uint8_t  scanner_edges;
    int8_t   scanner_rssi_dbm;     /* -120..-40 */
    bool     scanner_fast;
    int8_t   listen_rssi_dbm;      /* -120..-40 */
} subghz_rf_settings_t;

void subghz_rf_settings_defaults(subghz_rf_settings_t *out);
void subghz_rf_settings_load(subghz_rf_settings_t *out);
void subghz_rf_settings_save(const subghz_rf_settings_t *s);
void subghz_rf_settings_clamp(subghz_rf_settings_t *s);

void subghz_rf_build_hunter_cmd(const subghz_rf_settings_t *s, char *buf, size_t len);
void subghz_rf_build_scanner_cmd(const subghz_rf_settings_t *s, char *buf, size_t len);

/* Dropdown index helpers (settings UI) */
int  subghz_rf_hunter_trigger_index(int8_t dbm);
int8_t subghz_rf_hunter_trigger_from_index(int idx);

int  subghz_rf_hunter_timeout_index(uint16_t ms);
uint16_t subghz_rf_hunter_timeout_from_index(int idx);

int  subghz_rf_scanner_rssi_index(int8_t dbm);
int8_t subghz_rf_scanner_rssi_from_index(int idx);

int  subghz_rf_scanner_dwell_index(uint16_t ms);
uint16_t subghz_rf_scanner_dwell_from_index(int idx);

int  subghz_rf_scanner_edges_index(uint8_t edges);
uint8_t subghz_rf_scanner_edges_from_index(int idx);

int  subghz_rf_listen_rssi_index(int8_t dbm);
int8_t subghz_rf_listen_rssi_from_index(int idx);

#endif
