#include "subghz_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const s_signal_keys[] = {
    "idx=",
    "type=",
    "freq=",
    "bits=",
    "serial=",
    "btn=",
    "proto=",
    "learn=",
    "mf=",
    "cnt=",
    "te=",
    "edges=",
};

static const char *find_next_key(const char *value_start)
{
    const char *earliest = NULL;

    for (size_t i = 0; i < sizeof(s_signal_keys) / sizeof(s_signal_keys[0]); i++) {
        const char *candidate = strstr(value_start, s_signal_keys[i]);
        while (candidate) {
            if (candidate > value_start && candidate[-1] == ' ') {
                if (!earliest || candidate < earliest)
                    earliest = candidate;
                break;
            }
            candidate = strstr(candidate + 1, s_signal_keys[i]);
        }
    }

    return earliest;
}

static bool extract_field(const char *line, const char *key, char *buf, size_t buf_size)
{
    const char *value;
    const char *end;
    size_t len;

    if (!buf || buf_size == 0) return false;
    buf[0] = '\0';

    value = strstr(line, key);
    if (!value) return false;

    value += strlen(key);
    while (*value == ' ')
        value++;

    end = find_next_key(value);
    if (!end)
        end = value + strlen(value);

    while (end > value && end[-1] == ' ')
        end--;

    len = (size_t)(end - value);
    if (len >= buf_size)
        len = buf_size - 1;

    memcpy(buf, value, len);
    buf[len] = '\0';
    return len > 0;
}

static bool extract_int_field(const char *line, const char *key, int *out)
{
    char buf[32];
    char *end;

    if (!extract_field(line, key, buf, sizeof(buf))) return false;

    *out = (int)strtol(buf, &end, 0);
    return end != buf;
}

static bool extract_float_field(const char *line, const char *key, float *out)
{
    char buf[32];
    char *end;

    if (!extract_field(line, key, buf, sizeof(buf))) return false;

    *out = strtof(buf, &end);
    return end != buf;
}

const char *subghz_normalize_line(const char *line)
{
    const char *upper;
    const char *lower;

    if (!line) return NULL;

    upper = strstr(line, "[SUBGHZ_");
    lower = strstr(line, "[subghz]");
    if (!upper) return lower;
    if (!lower) return upper;
    return upper < lower ? upper : lower;
}

bool subghz_parse_rssi_line(const char *line, int *out_rssi)
{
    const char *tag;
    const char *value;
    char *end;

    if (!out_rssi) return false;

    tag = subghz_normalize_line(line);
    if (!tag || strncmp(tag, "[SUBGHZ_RSSI]", strlen("[SUBGHZ_RSSI]")) != 0)
        return false;

    value = strchr(tag, ']');
    if (!value) return false;

    value++;
    while (*value == ' ')
        value++;

    *out_rssi = (int)strtol(value, &end, 10);
    return end != value;
}

bool subghz_parse_signal_line(const char *line, subghz_signal_info_t *out)
{
    const char *tag;
    const char *fields;

    if (!out) return false;
    memset(out, 0, sizeof(*out));

    tag = subghz_normalize_line(line);
    if (!tag) return false;

    if (strncmp(tag, "[SUBGHZ_RX]", strlen("[SUBGHZ_RX]")) == 0) {
        out->kind = SUBGHZ_SIGNAL_KIND_RX;
    } else if (strncmp(tag, "[SUBGHZ_RX_DUP]", strlen("[SUBGHZ_RX_DUP]")) == 0) {
        out->kind = SUBGHZ_SIGNAL_KIND_RX_DUP;
        out->is_duplicate = true;
    } else if (strncmp(tag, "[SUBGHZ_RAW]", strlen("[SUBGHZ_RAW]")) == 0) {
        out->kind = SUBGHZ_SIGNAL_KIND_RAW;
        out->is_raw = true;
    } else if (strncmp(tag, "[SUBGHZ_LIST]", strlen("[SUBGHZ_LIST]")) == 0) {
        out->kind = SUBGHZ_SIGNAL_KIND_LIST;
    } else {
        return false;
    }

    fields = strchr(tag, ']');
    if (!fields) return false;

    fields++;
    while (*fields == ' ')
        fields++;

    extract_int_field(fields, "idx=", &out->idx);
    extract_field(fields, "type=", out->type, sizeof(out->type));
    extract_float_field(fields, "freq=", &out->freq);
    extract_int_field(fields, "bits=", &out->bits);
    extract_field(fields, "serial=", out->serial, sizeof(out->serial));
    extract_int_field(fields, "btn=", &out->btn);
    extract_field(fields, "proto=", out->proto, sizeof(out->proto));
    extract_field(fields, "learn=", out->learn, sizeof(out->learn));
    extract_field(fields, "mf=", out->mf, sizeof(out->mf));
    extract_int_field(fields, "cnt=", &out->cnt);
    extract_int_field(fields, "te=", &out->te);
    extract_int_field(fields, "edges=", &out->edges);

    if (out->kind == SUBGHZ_SIGNAL_KIND_RAW) {
        snprintf(out->type, sizeof(out->type), "RAW");
        if (out->edges > 0)
            snprintf(out->mf, sizeof(out->mf), "%de", out->edges);
    }

    return out->idx > 0 || out->kind == SUBGHZ_SIGNAL_KIND_RAW;
}