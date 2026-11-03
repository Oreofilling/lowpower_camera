/*
 * CT-Prov/1 communication.scan (protocol doc section 9): request parsing
 * and cursor-paged responses over a caller-gathered item array. The cursor
 * is the plain decimal index of the next item — opaque to the client, and
 * only meaningful against the array that produced it, which is why the
 * glue layer refreshes the radio scan only on cursor-less (first page)
 * requests.
 */
#include "ct_prov_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ct_prov_envelope.h"
#include "ct_prov_json.h"

#define CT_PROV_SCAN_RESULT_OP "communication.scan.result"

/* Worst-case bytes one encoded item can occupy: every SSID byte escaped to
 * \u00xx plus the fixed members and punctuation. */
#define CT_PROV_SCAN_ITEM_RESERVE 256
/* Trailer: "],"more":false,"cursor":""} plus slack. */
#define CT_PROV_SCAN_TAIL_RESERVE 48

static size_t error_response(int64_t id, const char *code, const char *message,
                             char *resp, size_t resp_cap)
{
    return ct_prov_envelope_encode_response(id, CT_PROV_SCAN_RESULT_OP, false,
                                            NULL, code, message, resp,
                                            resp_cap);
}

int ct_prov_scan_parse(const char *data_json, char *section,
                       size_t section_cap, bool *has_cursor, size_t *cursor)
{
    *has_cursor = false;
    *cursor = 0;
    section[0] = '\0';

    cJSON *root = cJSON_Parse(data_json != NULL ? data_json : "{}");
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *j_section = cJSON_GetObjectItem(root, "section");
    const char *sec = cJSON_GetStringValue(j_section);
    if (sec == NULL || sec[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }
    if (strlen(sec) >= section_cap) {
        cJSON_Delete(root);
        return -1;
    }
    strcpy(section, sec);

    const cJSON *j_cursor = cJSON_GetObjectItem(root, "cursor");
    if (j_cursor != NULL) {
        const char *cur = cJSON_GetStringValue(j_cursor);
        if (cur == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        if (cur[0] != '\0') {
            for (const char *p = cur; *p != '\0'; p++) {
                if (!isdigit((unsigned char)*p)) {
                    cJSON_Delete(root);
                    return -1;
                }
            }
            errno = 0;
            char *end = NULL;
            unsigned long long v = strtoull(cur, &end, 10);
            if (errno == ERANGE || end == cur || *end != '\0' ||
                v > (unsigned long long)(size_t)-1 / 2) {
                cJSON_Delete(root);
                return -1;
            }
            *cursor = (size_t)v;
            *has_cursor = true;
        }
    }

    cJSON_Delete(root);
    return 0;
}

size_t ct_prov_scan_page_response(int64_t id, const char *section,
                                  bool has_cursor, size_t cursor,
                                  const ct_prov_scan_item_t *items,
                                  size_t count, char *scratch,
                                  size_t scratch_cap, char *resp,
                                  size_t resp_cap)
{
    if (section == NULL || section[0] == '\0') {
        return error_response(id, "INVALID_ARGUMENT", "section required",
                              resp, resp_cap);
    }
    if (has_cursor && cursor > count) {
        /* Doc 9: a cursor past the array is stale (new scan ran meanwhile
         * or the cursor crossed connections). */
        return error_response(id, "INVALID_ARGUMENT", "stale cursor", resp,
                              resp_cap);
    }

    const size_t start = has_cursor ? cursor : 0;
    ct_emit_t e = {.buf = scratch, .cap = scratch_cap, .pos = 0,
                   .overflow = false};
    ct_emit_str(&e, "{\"section\":");
    ct_emit_json_string(&e, section);
    ct_emit_str(&e, ",\"items\":[");

    size_t emitted = 0;
    size_t next = start;
    while (next < count && emitted < CT_PROV_SCAN_PAGE_MAX) {
        if (e.pos + CT_PROV_SCAN_ITEM_RESERVE + CT_PROV_SCAN_TAIL_RESERVE >
            scratch_cap) {
            break; /* keep the trailer inside the buffer */
        }
        if (emitted > 0) {
            ct_emit_str(&e, ",");
        }
        const ct_prov_scan_item_t *item = &items[next];
        ct_emit_str(&e, "{\"network_id\":");
        ct_emit_json_string(&e, item->network_id);
        ct_emit_str(&e, ",\"display_name\":");
        ct_emit_json_string(&e, item->network_id);
        ct_emit_str(&e, ",\"signal\":");
        ct_emit_i64(&e, item->signal);
        if (item->has_channel) {
            ct_emit_str(&e, ",\"channel\":");
            ct_emit_i64(&e, item->channel);
        }
        ct_emit_str(&e, "}");
        emitted++;
        next++;
    }

    const bool more = next < count;
    ct_emit_str(&e, "],\"more\":");
    ct_emit_str(&e, more ? "true" : "false");
    ct_emit_str(&e, ",\"cursor\":");
    if (more) {
        char cur[24];
        snprintf(cur, sizeof(cur), "%zu", next);
        ct_emit_json_string(&e, cur);
    } else {
        ct_emit_str(&e, "\"\"");
    }
    ct_emit_str(&e, "}");

    if (e.overflow) {
        return error_response(id, "INTERNAL_ERROR",
                              "scan page overflows buffer", resp, resp_cap);
    }
    scratch[e.pos] = '\0';
    return ct_prov_envelope_encode_response(id, CT_PROV_SCAN_RESULT_OP, true,
                                            scratch, NULL, NULL, resp,
                                            resp_cap);
}
