/**
 * @file iot_recon.c
 * @brief Mesh Recon parsing (see iot_recon.h).
 */

#include "iot_recon.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

bool zig_get_value(const char *line, const char *key, char *out, size_t len)
{
    if (out && len) out[0] = '\0';
    if (!line || !key || !out || len == 0) return false;

    size_t klen = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        // Token boundary: key must start the line or follow a space, and be
        // immediately followed by '=' (so "rssi" never matches "best_rssi").
        bool left_ok = (p == line) || (*(p - 1) == ' ');
        if (left_ok && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t i = 0;
            while (v[i] && v[i] != ' ' && i < len - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
        p += klen;
    }
    return false;
}

int zig_get_int(const char *line, const char *key, int fallback)
{
    char buf[24];
    if (!zig_get_value(line, key, buf, sizeof(buf))) return fallback;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return fallback;
    return (int)v;
}

uint32_t zig_get_u32(const char *line, const char *key, uint32_t fallback)
{
    char buf[24];
    if (!zig_get_value(line, key, buf, sizeof(buf))) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 0);
    if (end == buf) return fallback;
    return (uint32_t)v;
}

zig_line_kind_t zig_line_kind(const char *line)
{
    if (!line) return ZIG_LINE_NONE;
    const char *p = strstr(line, "[ZIG]");
    if (!p) return ZIG_LINE_NONE;
    p += 5;
    while (*p == ' ') p++;
    if (strncmp(p, "status", 6) == 0) return ZIG_LINE_STATUS;
    if (strncmp(p, "pan", 3) == 0)    return ZIG_LINE_PAN;
    if (strncmp(p, "node", 4) == 0)   return ZIG_LINE_NODE;
    if (strncmp(p, "edge", 4) == 0)   return ZIG_LINE_EDGE;
    return ZIG_LINE_NONE;
}

void iot_recon_format_channels(uint32_t mask, char *out, size_t len)
{
    if (!out || len == 0) return;
    out[0] = '\0';
    size_t pos = 0;
    bool first = true;
    for (int i = 0; i < 32 && pos < len - 1; i++) {
        if (mask & (1u << i)) {
            int n = snprintf(out + pos, len - pos, "%s%d", first ? "" : ",", i);
            if (n < 0) break;
            pos += (size_t)n;
            first = false;
        }
    }
    if (first) snprintf(out, len, "-");
}

void iot_recon_humanize_age(uint32_t age_ms, char *out, size_t len)
{
    if (!out || len == 0) return;
    uint32_t s = age_ms / 1000;
    if (s < 60) snprintf(out, len, "%us", (unsigned)s);
    else        snprintf(out, len, "%um", (unsigned)(s / 60));
}

void iot_recon_parse_status(const char *line, iot_status_t *st)
{
    if (!st) return;
    st->active  = zig_get_int(line, "active", 0) != 0;
    st->channel = zig_get_int(line, "channel", 0);
    st->packets = zig_get_int(line, "packets", 0);
    st->pans    = zig_get_int(line, "pans", 0);
    st->dropped = zig_get_int(line, "dropped", 0);
}

bool iot_recon_parse_pan(const char *line, iot_pan_t *pan)
{
    if (!pan) return false;
    memset(pan, 0, sizeof(*pan));

    char kind[16] = {0};
    zig_get_value(line, "kind", kind, sizeof(kind));
    if (strcmp(kind, "broadcast") == 0) return false;   // UI hides these

    zig_get_value(line, "id", pan->pan_id, sizeof(pan->pan_id));
    zig_get_value(line, "proto", pan->proto, sizeof(pan->proto));
    zig_get_value(line, "confidence", pan->confidence, sizeof(pan->confidence));
    snprintf(pan->kind, sizeof(pan->kind), "%s", kind);
    pan->nodes     = zig_get_int(line, "nodes", 0);
    pan->packets   = zig_get_int(line, "packets", 0);
    pan->best_rssi = zig_get_int(line, "best_rssi", 0);
    pan->last_rssi = zig_get_int(line, "last_rssi", 0);
    pan->age_ms    = zig_get_u32(line, "age_ms", 0);
    uint32_t mask  = zig_get_u32(line, "channels", 0);
    iot_recon_format_channels(mask, pan->channels, sizeof(pan->channels));
    return true;
}

void iot_recon_parse_node(const char *line, iot_node_t *node)
{
    if (!node) return;
    memset(node, 0, sizeof(*node));

    zig_get_value(line, "pan", node->pan_id, sizeof(node->pan_id));
    zig_get_value(line, "role", node->role, sizeof(node->role));
    zig_get_value(line, "vendor", node->vendor, sizeof(node->vendor));
    zig_get_value(line, "device_hint", node->device_hint, sizeof(node->device_hint));
    zig_get_value(line, "battery", node->battery, sizeof(node->battery));
    node->packets      = zig_get_int(line, "packets", 0);
    node->last_rssi    = zig_get_int(line, "last_rssi", 0);
    node->best_rssi    = zig_get_int(line, "best_rssi", 0);
    node->avg_rssi     = zig_get_int(line, "avg_rssi", 0);
    node->lqi          = zig_get_int(line, "lqi", 0);
    node->sample_count = zig_get_int(line, "sample_count", 0);
    node->last_channel = zig_get_int(line, "last_channel", 0);
    node->age_ms       = zig_get_u32(line, "age_ms", 0);

    char addr_type[8] = {0}, shortv[16] = {0}, ext[24] = {0};
    zig_get_value(line, "addr_type", addr_type, sizeof(addr_type));
    zig_get_value(line, "short", shortv, sizeof(shortv));
    zig_get_value(line, "ext", ext, sizeof(ext));
    if (strcmp(addr_type, "ext") == 0 || strcmp(shortv, "na") == 0 || shortv[0] == '\0') {
        size_t el = strlen(ext);
        char last4[5] = {0};
        const char *src = (el >= 4) ? ext + el - 4 : ext;
        memcpy(last4, src, (el >= 4) ? 4 : el);
        snprintf(node->addr, sizeof(node->addr), "EXT ...%s", last4);
    } else {
        snprintf(node->addr, sizeof(node->addr), "%s", shortv);
    }
}

void iot_recon_parse_edge(const char *line, iot_edge_t *edge)
{
    if (!edge) return;
    memset(edge, 0, sizeof(*edge));
    zig_get_value(line, "pan", edge->pan_id, sizeof(edge->pan_id));
    zig_get_value(line, "from", edge->from, sizeof(edge->from));
    zig_get_value(line, "to", edge->to, sizeof(edge->to));
    zig_get_value(line, "kind", edge->kind, sizeof(edge->kind));
    edge->packets   = zig_get_int(line, "packets", 0);
    edge->last_rssi = zig_get_int(line, "last_rssi", 0);
    edge->age_ms    = zig_get_u32(line, "age_ms", 0);
}

void iot_recon_reset_all(iot_recon_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void iot_recon_reset_pans(iot_recon_t *r)
{
    if (r) r->pan_count = 0;
}

void iot_recon_reset_nodes_edges(iot_recon_t *r)
{
    if (!r) return;
    r->node_count = 0;
    r->edge_count = 0;
}

// Upsert: update an existing record (matched by identity) in place, or append
// a new one. This lets the live view accumulate/refresh rather than wiping and
// rebuilding on every poll.
void iot_recon_feed_line(iot_recon_t *r, const char *line)
{
    if (!r || !line) return;
    switch (zig_line_kind(line)) {
        case ZIG_LINE_STATUS:
            iot_recon_parse_status(line, &r->status);
            break;
        case ZIG_LINE_PAN: {
            iot_pan_t pan;
            if (!iot_recon_parse_pan(line, &pan)) break;        // skips broadcast
            for (int i = 0; i < r->pan_count; i++) {
                if (strcmp(r->pans[i].pan_id, pan.pan_id) == 0) {
                    r->pans[i] = pan;
                    return;
                }
            }
            if (r->pan_count < IOT_RECON_MAX_PANS) {
                r->pans[r->pan_count++] = pan;
            }
            break;
        }
        case ZIG_LINE_NODE: {
            iot_node_t node;
            iot_recon_parse_node(line, &node);
            for (int i = 0; i < r->node_count; i++) {
                if (strcmp(r->nodes[i].pan_id, node.pan_id) == 0 &&
                    strcmp(r->nodes[i].addr, node.addr) == 0) {
                    r->nodes[i] = node;
                    return;
                }
            }
            if (r->node_count < IOT_RECON_MAX_NODES) {
                r->nodes[r->node_count++] = node;
            }
            break;
        }
        case ZIG_LINE_EDGE: {
            iot_edge_t edge;
            iot_recon_parse_edge(line, &edge);
            for (int i = 0; i < r->edge_count; i++) {
                if (strcmp(r->edges[i].pan_id, edge.pan_id) == 0 &&
                    strcmp(r->edges[i].from, edge.from) == 0 &&
                    strcmp(r->edges[i].to, edge.to) == 0) {
                    r->edges[i] = edge;
                    return;
                }
            }
            if (r->edge_count < IOT_RECON_MAX_EDGES) {
                r->edges[r->edge_count++] = edge;
            }
            break;
        }
        default:
            break;
    }
}
