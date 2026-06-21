/**
 * @file subghz_rf_settings.c
 * @brief NVS-backed SubGHz RF settings plus UART command builders.
 *
 * Mirrors the coreS3 reference module verbatim — same NVS layout, same
 * dropdown option arrays, same generated CLI commands — so the JanOS
 * firmware sees an identical payload on both displays.
 */

#include "subghz_rf_settings.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "subghz_rf";

#define NVS_NS           "subghz_rf"
#define KEY_H_TRIG       "h_trig"
#define KEY_H_TMO        "h_tmo"
#define KEY_H_RAW        "h_raw"
#define KEY_H_SINGLE     "h_single"
#define KEY_H_FAST       "h_fast"
#define KEY_SC_DWELL     "sc_dwell"
#define KEY_SC_EDGES     "sc_edges"
#define KEY_SC_RSSI      "sc_rssi"
#define KEY_SC_FAST      "sc_fast"
#define KEY_LS_RSSI      "ls_rssi"

static const int8_t s_trigger_dbm[] = { -85, -80, -75, -70, -65, -60 };
static const uint16_t s_timeout_ms[] = { 1000, 2000, 3000, 5000 };
static const int8_t s_scanner_rssi[] = { -85, -80, -75, -70, -65, -60 };
static const uint16_t s_dwell_ms[] = { 40, 80, 120, 200 };
static const uint8_t s_edges[] = { 2, 4, 8, 12 };

void subghz_rf_settings_defaults(subghz_rf_settings_t *out)
{
    out->hunter_trigger_dbm = -70;
    out->hunter_timeout_ms  = 2000;
    out->hunter_raw         = false;
    out->hunter_single      = false;
    out->hunter_fast        = false;
    out->scanner_dwell_ms   = 80;
    out->scanner_edges      = 4;
    out->scanner_rssi_dbm   = -80;
    out->scanner_fast       = true;
    out->listen_rssi_dbm    = -80;
}

void subghz_rf_settings_clamp(subghz_rf_settings_t *s)
{
    if (s->hunter_trigger_dbm > -40) s->hunter_trigger_dbm = -40;
    if (s->hunter_trigger_dbm < -120) s->hunter_trigger_dbm = -120;
    if (s->hunter_timeout_ms < 100) s->hunter_timeout_ms = 100;
    if (s->hunter_timeout_ms > 60000) s->hunter_timeout_ms = 60000;
    if (s->scanner_dwell_ms < 20) s->scanner_dwell_ms = 20;
    if (s->scanner_dwell_ms > 500) s->scanner_dwell_ms = 500;
    if (s->scanner_edges < 1) s->scanner_edges = 1;
    if (s->scanner_rssi_dbm > -40) s->scanner_rssi_dbm = -40;
    if (s->scanner_rssi_dbm < -120) s->scanner_rssi_dbm = -120;
    if (s->listen_rssi_dbm > -40) s->listen_rssi_dbm = -40;
    if (s->listen_rssi_dbm < -120) s->listen_rssi_dbm = -120;
}

void subghz_rf_settings_load(subghz_rf_settings_t *out)
{
    subghz_rf_settings_defaults(out);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK)
        return;

    int8_t i8;
    uint8_t u8;
    uint16_t u16;

    if (nvs_get_i8(nvs, KEY_H_TRIG, &i8) == ESP_OK)
        out->hunter_trigger_dbm = i8;
    if (nvs_get_u16(nvs, KEY_H_TMO, &u16) == ESP_OK)
        out->hunter_timeout_ms = u16;
    if (nvs_get_u8(nvs, KEY_H_RAW, &u8) == ESP_OK)
        out->hunter_raw = (u8 != 0);
    if (nvs_get_u8(nvs, KEY_H_SINGLE, &u8) == ESP_OK)
        out->hunter_single = (u8 != 0);
    if (nvs_get_u8(nvs, KEY_H_FAST, &u8) == ESP_OK)
        out->hunter_fast = (u8 != 0);
    if (nvs_get_u16(nvs, KEY_SC_DWELL, &u16) == ESP_OK)
        out->scanner_dwell_ms = u16;
    if (nvs_get_u8(nvs, KEY_SC_EDGES, &u8) == ESP_OK)
        out->scanner_edges = u8;
    if (nvs_get_i8(nvs, KEY_SC_RSSI, &i8) == ESP_OK)
        out->scanner_rssi_dbm = i8;
    if (nvs_get_u8(nvs, KEY_SC_FAST, &u8) == ESP_OK)
        out->scanner_fast = (u8 != 0);
    if (nvs_get_i8(nvs, KEY_LS_RSSI, &i8) == ESP_OK)
        out->listen_rssi_dbm = i8;

    nvs_close(nvs);
    subghz_rf_settings_clamp(out);
}

void subghz_rf_settings_save(const subghz_rf_settings_t *s)
{
    subghz_rf_settings_t tmp = *s;
    subghz_rf_settings_clamp(&tmp);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_i8(nvs, KEY_H_TRIG, tmp.hunter_trigger_dbm);
    nvs_set_u16(nvs, KEY_H_TMO, tmp.hunter_timeout_ms);
    nvs_set_u8(nvs, KEY_H_RAW, tmp.hunter_raw ? 1 : 0);
    nvs_set_u8(nvs, KEY_H_SINGLE, tmp.hunter_single ? 1 : 0);
    nvs_set_u8(nvs, KEY_H_FAST, tmp.hunter_fast ? 1 : 0);
    nvs_set_u16(nvs, KEY_SC_DWELL, tmp.scanner_dwell_ms);
    nvs_set_u8(nvs, KEY_SC_EDGES, tmp.scanner_edges);
    nvs_set_i8(nvs, KEY_SC_RSSI, tmp.scanner_rssi_dbm);
    nvs_set_u8(nvs, KEY_SC_FAST, tmp.scanner_fast ? 1 : 0);
    nvs_set_i8(nvs, KEY_LS_RSSI, tmp.listen_rssi_dbm);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static int nearest_index_int(const int *vals, int n, int target)
{
    int best = 0;
    int best_diff = abs(vals[0] - target);
    for (int i = 1; i < n; i++) {
        int d = abs(vals[i] - target);
        if (d < best_diff) {
            best_diff = d;
            best = i;
        }
    }
    return best;
}

int subghz_rf_hunter_trigger_index(int8_t dbm)
{
    int vals[6];
    for (int i = 0; i < 6; i++) vals[i] = s_trigger_dbm[i];
    return nearest_index_int(vals, 6, dbm);
}

int8_t subghz_rf_hunter_trigger_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    return s_trigger_dbm[idx];
}

int subghz_rf_hunter_timeout_index(uint16_t ms)
{
    for (int i = 0; i < 4; i++)
        if (s_timeout_ms[i] == ms) return i;
    if (ms <= 1500) return 0;
    if (ms <= 2500) return 1;
    if (ms <= 4000) return 2;
    return 3;
}

uint16_t subghz_rf_hunter_timeout_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return s_timeout_ms[idx];
}

int subghz_rf_scanner_rssi_index(int8_t dbm)
{
    int vals[6];
    for (int i = 0; i < 6; i++) vals[i] = s_scanner_rssi[i];
    return nearest_index_int(vals, 6, dbm);
}

int8_t subghz_rf_scanner_rssi_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    return s_scanner_rssi[idx];
}

int subghz_rf_scanner_dwell_index(uint16_t ms)
{
    for (int i = 0; i < 4; i++)
        if (s_dwell_ms[i] == ms) return i;
    if (ms <= 60) return 0;
    if (ms <= 100) return 1;
    if (ms <= 160) return 2;
    return 3;
}

uint16_t subghz_rf_scanner_dwell_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return s_dwell_ms[idx];
}

int subghz_rf_scanner_edges_index(uint8_t edges)
{
    for (int i = 0; i < 4; i++)
        if (s_edges[i] == edges) return i;
    if (edges <= 3) return 0;
    if (edges <= 6) return 1;
    if (edges <= 10) return 2;
    return 3;
}

uint8_t subghz_rf_scanner_edges_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return s_edges[idx];
}

int subghz_rf_listen_rssi_index(int8_t dbm)
{
    int vals[6];
    for (int i = 0; i < 6; i++) vals[i] = s_scanner_rssi[i];
    return nearest_index_int(vals, 6, dbm);
}

int8_t subghz_rf_listen_rssi_from_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    return s_scanner_rssi[idx];
}

void subghz_rf_build_hunter_cmd(const subghz_rf_settings_t *s, char *buf, size_t len)
{
    subghz_rf_settings_t tmp = *s;
    subghz_rf_settings_clamp(&tmp);

    int n = snprintf(buf, len, "subghz_freq_analyzer %d", (int)tmp.hunter_trigger_dbm);
    if (n < 0 || (size_t)n >= len) {
        buf[0] = '\0';
        return;
    }

    if (tmp.hunter_raw) {
        n += snprintf(buf + n, len - (size_t)n, " raw");
    } else {
        n += snprintf(buf + n, len - (size_t)n, " hunt");
        if (tmp.hunter_single)
            n += snprintf(buf + n, len - (size_t)n, " single");
    }
    if (tmp.hunter_fast)
        n += snprintf(buf + n, len - (size_t)n, " fast");
    snprintf(buf + n, len - (size_t)n, " timeout=%u", (unsigned)tmp.hunter_timeout_ms);
}

void subghz_rf_build_scanner_cmd(const subghz_rf_settings_t *s, char *buf, size_t len)
{
    subghz_rf_settings_t tmp = *s;
    subghz_rf_settings_clamp(&tmp);

    int n = snprintf(buf, len, "subghz_scanner dwell=%u edges=%u %d",
                     (unsigned)tmp.scanner_dwell_ms,
                     (unsigned)tmp.scanner_edges,
                     (int)tmp.scanner_rssi_dbm);
    if (n < 0 || (size_t)n >= len) {
        buf[0] = '\0';
        return;
    }
    if (tmp.scanner_fast)
        snprintf(buf + n, len - (size_t)n, " fast");
}
