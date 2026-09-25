/**
 * @file nfc_parser.c
 * @brief Shared parser for JanOS NFC UART responses
 */

#include "nfc_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void nfc_card_reset(nfc_ui_card_t *card)
{
    if (card) memset(card, 0, sizeof(*card));
}

bool nfc_line_is_end(const char *line)
{
    return line &&
           (strcmp(line, "[NFC] END") == 0 ||
            strcmp(line, "[NFC] END\r") == 0);
}

static void copy_after(char *out, size_t out_len,
                       const char *line, size_t prefix_len)
{
    if (out && out_len) snprintf(out, out_len, "%s", line + prefix_len);
}

bool nfc_parse_card_line(const char *line, nfc_ui_card_t *card)
{
    if (!line || !card || strncmp(line, "[NFC]", 5) != 0) return false;

    if (nfc_line_is_end(line)) return true;

    if (strncmp(line, "[NFC] type: ", 12) == 0) {
        copy_after(card->type, sizeof(card->type), line, 12);
    } else if (strncmp(line, "[NFC] uid: ", 11) == 0) {
        copy_after(card->uid, sizeof(card->uid), line, 11);
    } else if (strncmp(line, "[NFC] atqa: ", 12) == 0) {
        unsigned a0 = 0, a1 = 0, sak = 0;
        if (sscanf(line, "[NFC] atqa: %02X %02X  sak: %02X",
                   &a0, &a1, &sak) == 3) {
            card->atqa[0] = (uint8_t)a0;
            card->atqa[1] = (uint8_t)a1;
            card->sak = (uint8_t)sak;
            card->have_atqa_sak = true;
        }
    } else if (strncmp(line, "[NFC] idm: ", 11) == 0) {
        copy_after(card->idm, sizeof(card->idm), line, 11);
        card->have_idm = true;
    } else if (strncmp(line, "[NFC] data: ", 12) == 0) {
        if (sscanf(line, "[NFC] data: %u bytes, %u blocks x %u",
                   &card->data_len, &card->num_blocks,
                   &card->block_size) >= 1) {
            card->have_data = true;
        }
    } else if (strstr(line, "[NFC] no card detected")) {
        card->no_card = true;
    } else if (strstr(line, "[NFC] not detected")) {
        card->not_detected = true;
    } else if (strstr(line, "[NFC] detected")) {
        card->detected = true;
    } else if (strstr(line, "[NFC] not initialized")) {
        card->not_initialized = true;
    } else if (strstr(line, "[NFC] nothing to save")) {
        card->nothing_to_save = true;
    } else if (strstr(line, "[NFC] save failed")) {
        card->save_failed = true;
    } else if (strstr(line, "[NFC] load failed") ||
               strstr(line, "[NFC] not found")) {
        card->load_failed = true;
    } else if (strstr(line, "[NFC] emulate failed")) {
        card->emulate_failed = true;
    } else if (strstr(line, "[NFC] no card loaded")) {
        card->no_card_loaded = true;
    } else if (strstr(line, "[NFC] rename failed")) {
        card->rename_failed = true;
    } else if (strstr(line, "[NFC] delete failed")) {
        card->delete_failed = true;
    } else if (strncmp(line, "[NFC] saved: ", 13) == 0) {
        copy_after(card->saved_path, sizeof(card->saved_path), line, 13);
        card->have_saved = true;
    } else if (strncmp(line, "[NFC] loaded: ", 14) == 0) {
        copy_after(card->loaded_path, sizeof(card->loaded_path), line, 14);
        card->have_loaded = true;
    } else if (strncmp(line, "[NFC] renamed: ", 15) == 0) {
        const char *target = strstr(line + 15, " -> ");
        copy_after(card->renamed_path, sizeof(card->renamed_path),
                   target ? target : line, target ? (size_t)(target + 4 - line) : 15);
        card->have_renamed = true;
    } else if (strncmp(line, "[NFC] deleted: ", 15) == 0) {
        copy_after(card->deleted_path, sizeof(card->deleted_path), line, 15);
        card->have_deleted = true;
    } else if (strncmp(line, "[NFC] emulating ", 16) == 0) {
        copy_after(card->emulating_summary, sizeof(card->emulating_summary), line, 16);
        char *suffix = strstr(card->emulating_summary, ". Use ");
        if (suffix) *suffix = '\0';
        card->have_emulating = true;
    } else if (strstr(line, "full NTAG/Ultralight page data emulated")) {
        card->emulate_full_ul = true;
    } else if (strstr(line, "UID/ATQA/SAK level only")) {
        card->emulate_uid_only = true;
    }

    return true;
}

bool nfc_parse_list_entry(const char *line, nfc_list_entry_t *entry)
{
    if (!line || !entry || strstr(line, "[NFC]")) return false;

    int idx = -1;
    char name[64];
    char extra = '\0';
    if (sscanf(line, "%d %63s %c", &idx, name, &extra) != 2 || idx < 0) {
        return false;
    }
    size_t len = strlen(name);
    if (len < 4 || strcasecmp(name + len - 4, ".nfc") != 0) return false;

    entry->idx = idx;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    return true;
}

bool nfc_parse_card_count(const char *line, int *count)
{
    int parsed = 0;
    if (!line || !count ||
        sscanf(line, "[NFC] %d card(s)", &parsed) != 1) {
        return false;
    }
    *count = parsed;
    return true;
}

bool nfc_parse_bus_line(const char *line, int *mode)
{
    if (!line || !mode) return false;
    if (strstr(line, "[NFC] bus: spi")) {
        *mode = 0;
    } else if (strstr(line, "[NFC] bus: i2c")) {
        *mode = 1;
    } else if (strstr(line, "[NFC] bus: pn532")) {
        *mode = 2;
    } else {
        return false;
    }
    return true;
}

bool nfc_name_is_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; ++p) {
        if (isalnum((unsigned char)*p) ||
            *p == '_' || *p == '-' || *p == '.') {
            continue;
        }
        return false;
    }
    return true;
}

void nfc_path_basename(const char *path, char *out, size_t out_len,
                       bool strip_nfc)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    const char *slash = strrchr(path, '/');
    snprintf(out, out_len, "%s", slash ? slash + 1 : path);

    if (strip_nfc) {
        size_t len = strlen(out);
        if (len > 4 && strcmp(out + len - 4, ".nfc") == 0) {
            out[len - 4] = '\0';
        }
    }
}

void nfc_format_card_detail(const nfc_ui_card_t *card,
                            char *status, size_t status_len,
                            char *detail, size_t detail_len)
{
    if (status && status_len) status[0] = '\0';
    if (detail && detail_len) detail[0] = '\0';
    if (!card) return;

    if (status && status_len) {
        snprintf(status, status_len, "%s",
                 card->type[0] ? card->type : "Card");
    }
    if (!detail || detail_len == 0) return;

    if (card->have_atqa_sak) {
        snprintf(detail, detail_len,
                 "UID %s\nATQA %02X %02X SAK %02X%s",
                 card->uid[0] ? card->uid : "--",
                 card->atqa[0], card->atqa[1], card->sak,
                 card->have_data ? "\nData dumped" : "");
    } else if (card->have_idm) {
        snprintf(detail, detail_len, "UID %s\nIDm %s%s",
                 card->uid[0] ? card->uid : "--", card->idm,
                 card->have_data ? "\nData dumped" : "");
    } else {
        snprintf(detail, detail_len, "UID %s%s",
                 card->uid[0] ? card->uid : "--",
                 card->have_data ? "\nData dumped" : "");
    }
}

bool nfc_type_is_classic(const char *type)
{
    return type && strstr(type, "Classic");
}

bool nfc_type_is_ultralight(const char *type)
{
    return type &&
           (strstr(type, "Ultralight") ||
            strstr(type, "NTAG") ||
            strstr(type, "MF0UL"));
}
