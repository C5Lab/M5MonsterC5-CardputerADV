/**
 * @file subghz_ext_client.c
 * @brief UART client for Monster subghz_ext_* commands.
 */

#include "subghz_ext_client.h"
#include "uart_handler.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SUBGHZ_EXT";

#define LOAD_PER_LINE  20
#define CMD_MAX        256

static SemaphoreHandle_t s_mtx;

static void ensure_mtx(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
}

typedef struct {
    bool license;
    bool decode_ok;
    bool saw_end;
    bool save_ok;
    bool encode_ok;
    bool info_end;
    bool tx_ok;
    bool tx_err;
    int mem_idx;
    int enc_count;
    int32_t *enc_buf;
    int enc_cap;
    int32_t *raw_buf;
    int raw_count;
    int raw_cap;
    float tx_freq;
    int tx_reps;
    subghz_ext_decode_t *dec;
    subghz_ext_info_t *info;
} ext_collect_t;

static bool extract_field(const char *line, const char *key, char *buf, size_t buf_size)
{
    const char *value = strstr(line, key);
    if (!value || !buf || buf_size == 0) {
        return false;
    }
    value += strlen(key);
    while (*value == ' ') {
        value++;
    }
    size_t n = 0;
    while (value[n] && value[n] != ' ' && n + 1 < buf_size) {
        n++;
    }
    memcpy(buf, value, n);
    buf[n] = '\0';
    return n > 0;
}

static void parse_decode_line(const char *line, subghz_ext_decode_t *out)
{
    memset(out, 0, sizeof(*out));
    extract_field(line, "type=", out->type, sizeof(out->type));
    extract_field(line, "proto=", out->proto, sizeof(out->proto));
    extract_field(line, "id=", out->id, sizeof(out->id));
    extract_field(line, "serial=", out->serial, sizeof(out->serial));
    extract_field(line, "mf=", out->mf, sizeof(out->mf));
    extract_field(line, "learn=", out->learn, sizeof(out->learn));

    char tmp[32];
    if (extract_field(line, "bits=", tmp, sizeof(tmp))) {
        out->bits = atoi(tmp);
    }
    if (extract_field(line, "te=", tmp, sizeof(tmp))) {
        out->te = atoi(tmp);
    }
    if (extract_field(line, "btn=", tmp, sizeof(tmp))) {
        out->btn = atoi(tmp);
    }
    if (extract_field(line, "cnt=", tmp, sizeof(tmp))) {
        out->cnt = atoi(tmp);
    }
    if (extract_field(line, "freq=", tmp, sizeof(tmp))) {
        out->freq = strtof(tmp, NULL);
    }
    if (extract_field(line, "seed=", tmp, sizeof(tmp))) {
        out->seed = (uint32_t)strtoul(tmp, NULL, 0);
    }
    out->rolling = (out->serial[0] != '\0');
    out->ok = true;
}

static void parse_info_line(const char *line, subghz_ext_info_t *out)
{
    memset(out, 0, sizeof(*out));
    extract_field(line, "type=", out->type, sizeof(out->type));
    extract_field(line, "serial=", out->serial, sizeof(out->serial));
    extract_field(line, "id=", out->id, sizeof(out->id));
    extract_field(line, "mf=", out->mf, sizeof(out->mf));
    extract_field(line, "learn=", out->learn, sizeof(out->learn));
    extract_field(line, "name=", out->name, sizeof(out->name));

    char tmp[32];
    if (extract_field(line, "bits=", tmp, sizeof(tmp))) {
        out->bits = atoi(tmp);
    }
    if (extract_field(line, "te=", tmp, sizeof(tmp))) {
        out->te = atoi(tmp);
    }
    if (extract_field(line, "btn=", tmp, sizeof(tmp))) {
        out->btn = atoi(tmp);
    }
    if (extract_field(line, "cnt=", tmp, sizeof(tmp))) {
        out->cnt = atoi(tmp);
    }
    if (extract_field(line, "edges=", tmp, sizeof(tmp))) {
        out->edges = atoi(tmp);
    }
    if (extract_field(line, "freq=", tmp, sizeof(tmp))) {
        out->freq = strtof(tmp, NULL);
    }
    out->is_raw = (strcmp(out->type, "RAW") == 0);
    out->rolling = (out->serial[0] != '\0');
}

static void collect_cb(const char *line, void *user)
{
    ext_collect_t *c = (ext_collect_t *)user;
    if (!c || !line) {
        return;
    }

    if (strstr(line, "[SUBGHZ_EXT_ERR]") && strstr(line, "license")) {
        c->license = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_DECODE] ")) {
        if (c->dec) {
            parse_decode_line(line, c->dec);
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_DECODE_END]")) {
        c->saw_end = true;
        c->decode_ok = (strstr(line, "ok=1") != NULL);
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_SAVE_ERR]")) {
        c->save_ok = false;
        c->saw_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_SAVE]")) {
        c->save_ok = true;
        const char *p = strstr(line, "mem_idx=");
        if (p) {
            c->mem_idx = atoi(p + 8);
        }
        c->saw_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_T]")) {
        const char *p = strstr(line, "]");
        if (!p || !c->enc_buf) {
            return;
        }
        p++;
        while (*p) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            if (c->enc_count < c->enc_cap) {
                c->enc_buf[c->enc_count++] = (int32_t)v;
            }
            p = end;
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_ENCODE_ERR]")) {
        c->encode_ok = false;
        c->saw_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_ENCODE_END]")) {
        c->encode_ok = (strstr(line, "count=0") == NULL);
        c->saw_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_INFO_RAW]")) {
        const char *p = strstr(line, "]");
        if (!p || !c->raw_buf) {
            return;
        }
        p++;
        while (*p) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            if (c->raw_count < c->raw_cap) {
                c->raw_buf[c->raw_count++] = (int32_t)v;
            }
            p = end;
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_INFO_ERR]")) {
        c->info_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_INFO_END]")) {
        c->info_end = true;
        return;
    }
    if (strstr(line, "[SUBGHZ_INFO]") && c->info) {
        parse_info_line(line, c->info);
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_TX_ERR]")) {
        c->tx_err = true;
        c->tx_ok = false;
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_TX] ")) {
        char tmp[32];
        if (extract_field(line, "freq=", tmp, sizeof(tmp))) {
            c->tx_freq = strtof(tmp, NULL);
        }
        if (extract_field(line, "reps=", tmp, sizeof(tmp))) {
            c->tx_reps = atoi(tmp);
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_EXT_TX_END]")) {
        c->saw_end = true;
        c->tx_ok = !c->tx_err && (strstr(line, "count=0") == NULL);
        return;
    }
}

subghz_ext_status_t subghz_ext_reset(void)
{
    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    uart_collect_begin("[SUBGHZ_EXT_RESET]", collect_cb, &c);
    uart_send_command("subghz_ext_reset");
    esp_err_t w = uart_collect_wait(1500);

    xSemaphoreGive(s_mtx);
    if (c.license) {
        return SUBGHZ_EXT_ERR_LICENSE;
    }
    return (w == ESP_OK) ? SUBGHZ_EXT_OK : SUBGHZ_EXT_ERR_TIMEOUT;
}

subghz_ext_status_t subghz_ext_decode(const int32_t *timings, int count,
                                      float freq_mhz, subghz_ext_decode_t *out)
{
    if (!timings || count < 2 || !out) {
        return SUBGHZ_EXT_ERR_NO_DECODE;
    }

    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    uart_collect_begin("[SUBGHZ_EXT_RESET]", collect_cb, &c);
    uart_send_command("subghz_ext_reset");
    uart_collect_wait(1500);
    if (c.license) {
        xSemaphoreGive(s_mtx);
        return SUBGHZ_EXT_ERR_LICENSE;
    }

    char cmd[CMD_MAX];
    int i = 0;
    while (i < count) {
        int pos = snprintf(cmd, sizeof(cmd), "subghz_ext_load");
        int chunk = 0;
        while (i < count && chunk < LOAD_PER_LINE && pos < (int)sizeof(cmd) - 12) {
            int n = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " %ld", (long)timings[i]);
            if (n < 0 || pos + n >= (int)sizeof(cmd) - 1) {
                break;
            }
            pos += n;
            i++;
            chunk++;
        }
        uart_send_command(cmd);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    memset(&c, 0, sizeof(c));
    c.dec = out;
    uart_collect_begin("[SUBGHZ_EXT_DECODE_END]", collect_cb, &c);
    snprintf(cmd, sizeof(cmd), "subghz_ext_decode %.2f", freq_mhz);
    uart_send_command(cmd);
    esp_err_t w = uart_collect_wait(4000);

    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "decode %s edges=%d freq=%.2f",
             (w == ESP_OK && c.decode_ok) ? "ok" : "fail", count, freq_mhz);
    if (c.license) {
        return SUBGHZ_EXT_ERR_LICENSE;
    }
    if (w != ESP_OK) {
        return SUBGHZ_EXT_ERR_TIMEOUT;
    }
    if (!c.decode_ok) {
        return SUBGHZ_EXT_ERR_NO_DECODE;
    }
    return SUBGHZ_EXT_OK;
}

subghz_ext_status_t subghz_ext_save(int *mem_idx_out)
{
    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    uart_collect_begin("[SUBGHZ_EXT_SAVE]", collect_cb, &c);
    uart_send_command("subghz_ext_save");
    esp_err_t w = uart_collect_wait(5000);

    xSemaphoreGive(s_mtx);

    if (c.license) {
        return SUBGHZ_EXT_ERR_LICENSE;
    }
    if (w != ESP_OK && !c.saw_end) {
        return SUBGHZ_EXT_ERR_TIMEOUT;
    }
    if (!c.save_ok) {
        return SUBGHZ_EXT_ERR_SAVE;
    }
    if (mem_idx_out) {
        *mem_idx_out = c.mem_idx;
    }
    return SUBGHZ_EXT_OK;
}

subghz_ext_status_t subghz_ext_encode(const char *cmd, int32_t *timings_out, int *count_out)
{
    if (!cmd || !timings_out || !count_out) {
        return SUBGHZ_EXT_ERR_ENCODE;
    }

    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    c.enc_buf = timings_out;
    c.enc_cap = SUBGHZ_EXT_MAX_TIMINGS;
    uart_collect_begin("[SUBGHZ_EXT_ENCODE_END]", collect_cb, &c);
    uart_send_command(cmd);
    esp_err_t w = uart_collect_wait(5000);

    xSemaphoreGive(s_mtx);

    if (c.license) {
        return SUBGHZ_EXT_ERR_LICENSE;
    }
    if (w != ESP_OK) {
        return SUBGHZ_EXT_ERR_TIMEOUT;
    }
    if (!c.encode_ok || c.enc_count < 2) {
        return SUBGHZ_EXT_ERR_ENCODE;
    }
    *count_out = c.enc_count;
    return SUBGHZ_EXT_OK;
}

subghz_ext_status_t subghz_ext_info_sd(int idx, subghz_ext_info_t *out,
                                       int32_t *raw_out, int raw_cap, int *raw_count)
{
    if (!out || idx < 1) {
        return SUBGHZ_EXT_ERR_INFO;
    }

    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    c.info = out;
    c.raw_buf = raw_out;
    c.raw_cap = (raw_out && raw_cap > 0) ? raw_cap : 0;
    uart_collect_begin("[SUBGHZ_INFO_END]", collect_cb, &c);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "subghz_info %d sd", idx);
    uart_send_command(cmd);
    esp_err_t w = uart_collect_wait(4000);

    if (raw_count) {
        *raw_count = c.raw_count;
    }
    xSemaphoreGive(s_mtx);

    if (w != ESP_OK && !c.info_end) {
        return SUBGHZ_EXT_ERR_TIMEOUT;
    }
    if (!out->type[0]) {
        return SUBGHZ_EXT_ERR_INFO;
    }
    return SUBGHZ_EXT_OK;
}

static bool name_is_safe_token(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

subghz_ext_status_t subghz_ext_tx_sd(int idx, const char *name,
                                     int32_t *timings_out, int *count_out,
                                     float *freq_out, int *reps_out)
{
    if (!timings_out || !count_out) {
        return SUBGHZ_EXT_ERR_ENCODE;
    }
    if (!name_is_safe_token(name) && idx < 1) {
        return SUBGHZ_EXT_ERR_ENCODE;
    }

    ensure_mtx();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    ext_collect_t c = {0};
    c.enc_buf = timings_out;
    c.enc_cap = SUBGHZ_EXT_MAX_TIMINGS;
    uart_collect_begin("[SUBGHZ_EXT_TX_END]", collect_cb, &c);

    char cmd[96];
    if (name_is_safe_token(name)) {
        snprintf(cmd, sizeof(cmd), "subghz_ext_tx sd %s", name);
    } else {
        snprintf(cmd, sizeof(cmd), "subghz_ext_tx sd %d", idx);
    }
    uart_send_command(cmd);
    esp_err_t w = uart_collect_wait(8000);

    xSemaphoreGive(s_mtx);

    if (c.license) {
        return SUBGHZ_EXT_ERR_LICENSE;
    }
    if (w != ESP_OK && !c.saw_end) {
        return SUBGHZ_EXT_ERR_TIMEOUT;
    }
    if (!c.tx_ok || c.enc_count < 2) {
        return SUBGHZ_EXT_ERR_ENCODE;
    }
    *count_out = c.enc_count;
    if (freq_out) {
        *freq_out = c.tx_freq;
    }
    if (reps_out) {
        *reps_out = c.tx_reps > 0 ? c.tx_reps : 3;
    }
    return SUBGHZ_EXT_OK;
}
