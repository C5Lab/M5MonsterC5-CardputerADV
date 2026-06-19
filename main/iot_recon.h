/**
 * @file iot_recon.h
 * @brief Mesh Recon (802.15.4 / Zigbee / Thread) data models and [ZIG] parse
 *        helpers. Pure C (no ESP/UI deps), mirrors the Tab5 caps.
 *
 * Protocol (board firmware unchanged, reached over UART):
 *   start_zig_recon all 250
 *   zig_recon_status / zig_recon_list all / zig_recon_nodes all
 *   stop / zig_recon_clear
 * Every recon line starts with "[ZIG]" and carries key=value tokens.
 */

#ifndef IOT_RECON_H
#define IOT_RECON_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define IOT_RECON_MAX_PANS  32
#define IOT_RECON_MAX_NODES 128
#define IOT_RECON_MAX_EDGES 128

typedef struct {
    char     pan_id[12];
    char     proto[16];
    char     confidence[12];
    char     kind[16];
    char     channels[40];      // humanized channel list from the bitmask
    int      nodes;
    int      packets;
    int      best_rssi;
    int      last_rssi;
    uint32_t age_ms;
} iot_pan_t;

typedef struct {
    char     pan_id[12];
    char     addr[24];          // display addr: "short" value or "EXT ...<last4>"
    char     role[16];
    char     vendor[20];
    char     device_hint[24];
    char     battery[12];
    int      packets;
    int      last_rssi;
    int      best_rssi;
    int      avg_rssi;
    int      lqi;
    int      sample_count;
    int      last_channel;
    uint32_t age_ms;
} iot_node_t;

typedef struct {
    char     pan_id[12];
    char     from[24];
    char     to[24];
    char     kind[16];
    int      packets;
    int      last_rssi;
    uint32_t age_ms;
} iot_edge_t;

typedef struct {
    bool active;
    int  channel;
    int  packets;
    int  pans;
    int  dropped;
} iot_status_t;

typedef struct {
    iot_status_t status;
    iot_pan_t    pans[IOT_RECON_MAX_PANS];
    int          pan_count;
    iot_node_t   nodes[IOT_RECON_MAX_NODES];
    int          node_count;
    iot_edge_t   edges[IOT_RECON_MAX_EDGES];
    int          edge_count;
} iot_recon_t;

typedef enum {
    ZIG_LINE_NONE = 0,
    ZIG_LINE_STATUS,
    ZIG_LINE_PAN,
    ZIG_LINE_NODE,
    ZIG_LINE_EDGE,
} zig_line_kind_t;

// ---- Low-level token helpers (like the Tab5 iot_recon_get_*) --------------

/** Copy the value of key=<value> (read until whitespace) into out. */
bool     zig_get_value(const char *line, const char *key, char *out, size_t len);
/** Parse key=<int> (base 10), or return fallback if missing/invalid. */
int      zig_get_int(const char *line, const char *key, int fallback);
/** Parse key=<u32> (base 0, accepts 0x..), or return fallback. */
uint32_t zig_get_u32(const char *line, const char *key, uint32_t fallback);

// ---- Line dispatch / parsing ----------------------------------------------

zig_line_kind_t zig_line_kind(const char *line);

void iot_recon_parse_status(const char *line, iot_status_t *st);
/** @return false (skip) for kind=broadcast PANs, else fills pan. */
bool iot_recon_parse_pan(const char *line, iot_pan_t *pan);
void iot_recon_parse_node(const char *line, iot_node_t *node);
void iot_recon_parse_edge(const char *line, iot_edge_t *edge);

// ---- Cache management -------------------------------------------------------

void iot_recon_reset_all(iot_recon_t *r);
void iot_recon_reset_pans(iot_recon_t *r);        // optional explicit clear
void iot_recon_reset_nodes_edges(iot_recon_t *r); // optional explicit clear

/**
 * Dispatch one line and upsert into the matching cache: an existing record is
 * refreshed in place (PAN by id, node by pan+addr, edge by pan+from+to), a new
 * one is appended (respecting caps). Lets the live view accumulate without
 * wiping/rebuilding each poll.
 */
void iot_recon_feed_line(iot_recon_t *r, const char *line);

// ---- Formatting helpers -----------------------------------------------------

/** Format a channel bitmask to a comma-separated list of set channel numbers. */
void iot_recon_format_channels(uint32_t mask, char *out, size_t len);
/** Humanize age_ms to "Ns" / "Nm". */
void iot_recon_humanize_age(uint32_t age_ms, char *out, size_t len);

#endif // IOT_RECON_H
