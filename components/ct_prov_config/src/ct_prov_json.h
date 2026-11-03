#ifndef __CT_PROV_JSON_H__
#define __CT_PROV_JSON_H__

/*
 * Minimal JSON emitter shared by the ct_prov_config sources. Twin of the
 * static helpers in ct_prov_envelope.c; kept local so the F1-tested
 * envelope layer stays untouched. Overflow never writes out of bounds:
 * every helper becomes a no-op once the flag is set.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    bool overflow;
} ct_emit_t;

static inline void ct_emit_raw(ct_emit_t *e, const char *s, size_t len)
{
    if (e->overflow) {
        return;
    }
    if (e->pos + len + 1 > e->cap) { /* keep room for the NUL */
        e->overflow = true;
        return;
    }
    memcpy(e->buf + e->pos, s, len);
    e->pos += len;
}

static inline void ct_emit_str(ct_emit_t *e, const char *s)
{
    ct_emit_raw(e, s, strlen(s));
}

static inline void ct_emit_i64(ct_emit_t *e, int64_t v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    ct_emit_str(e, tmp);
}

/* Canonical JSON escaping: quote, backslash and control characters; UTF-8
 * bytes pass through verbatim (same policy as the envelope encoder). */
static inline void ct_emit_json_string(ct_emit_t *e, const char *s)
{
    ct_emit_str(e, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        const unsigned char c = *p;
        switch (c) {
        case '"':
            ct_emit_str(e, "\\\"");
            continue;
        case '\\':
            ct_emit_str(e, "\\\\");
            continue;
        default:
            if (c < 0x20) {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                ct_emit_str(e, esc);
            } else {
                ct_emit_raw(e, (const char *)&c, 1);
            }
        }
    }
    ct_emit_str(e, "\"");
}

#endif /* __CT_PROV_JSON_H__ */
