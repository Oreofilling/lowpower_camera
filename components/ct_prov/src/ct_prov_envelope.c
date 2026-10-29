/*
 * CT-Prov/1 envelope layer implementation (protocol doc section 6).
 *
 * Decode pipeline and rejection order mirror the Dart reference exactly:
 * bom -> invalid_utf8 -> not_object -> malformed(structure) ->
 * trailing_garbage -> malformed(JSON) -> float_int -> invalid_shape.
 *
 * Why a hand-rolled top-level scanner next to cJSON: the protocol forbids
 * writing id/seq as floats ("1e18" and "1.0" are both rejected by the Dart
 * decoder), but cJSON collapses every number to a double and loses the
 * literal. The scanner walks the top-level members on the RAW text and
 * extracts id/seq as exact int64 tokens, plus verbatim slices of data/error
 * so re-encoding stays byte-exact. cJSON validates the document and provides
 * the decoded string fields (op/event/error.code/error.message).
 */
#include "ct_prov_envelope.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const char *k_env_err_names[CT_PROV_ENV_ERR__COUNT] = {
    "bom", "not_object", "float_int", "trailing_garbage",
    "malformed", "invalid_utf8", "invalid_shape",
};

const char *ct_prov_envelope_error_name(ct_prov_envelope_error_t err)
{
    if ((unsigned)err >= CT_PROV_ENV_ERR__COUNT) {
        return NULL;
    }
    return k_env_err_names[err];
}

void ct_prov_envelope_free(ct_prov_envelope_t *e)
{
    if (e == NULL) {
        return;
    }
    free(e->op);
    free(e->event);
    free(e->data_json);
    free(e->error_code);
    free(e->error_message);
    memset(e, 0, sizeof(*e));
}

/* ---- byte helpers ---------------------------------------------------- */

static bool is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Strict UTF-8 validation: rejects overlong forms, surrogates, and code
 * points above U+10FFFF. */
static bool utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        const uint8_t b = s[i];
        if (b < 0x80) {
            i++;
            continue;
        }
        int cont;
        uint32_t cp;
        if ((b & 0xE0) == 0xC0) {
            cont = 1;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            cont = 2;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            cont = 3;
            cp = b & 0x07;
        } else {
            return false;
        }
        if (i + (size_t)cont >= len) {
            return false;
        }
        for (int k = 1; k <= cont; k++) {
            const uint8_t cb = s[i + (size_t)k];
            if ((cb & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (cont == 1 && cp < 0x80) {
            return false;
        }
        if (cont == 2 && cp < 0x800) {
            return false;
        }
        if (cont == 3 && cp < 0x10000) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        i += (size_t)cont + 1;
    }
    return true;
}

/* Skip a JSON string starting at s[*i] == '"'; returns false on structural
 * surprise (well-formed input never trips this). */
static bool skip_string(const char *s, size_t len, size_t *i)
{
    (*i)++; /* opening quote */
    while (*i < len) {
        const char c = s[*i];
        if (c == '\\') {
            *i += 2;
            continue;
        }
        if (c == '"') {
            (*i)++;
            return true;
        }
        (*i)++;
    }
    return false;
}

/* Index just past the closing brace of the object opening at s[start];
 * (size_t)-1 on mismatch or EOF. Mirrors the Dart _scanObjectEnd. */
static size_t scan_object_end(const char *s, size_t len, size_t start)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = start; i < len; i++) {
        const char c = s[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) {
                return i + 1;
            }
            if (depth < 0) {
                return (size_t)-1;
            }
        }
    }
    return (size_t)-1;
}

/* ---- top-level member scanner ---------------------------------------- */

#define CT_PROV_MAX_TOP_MEMBERS 16

typedef struct {
    const char *key;   /* raw text between the key quotes (no decoding) */
    size_t key_len;
    const char *val;   /* raw value slice */
    size_t val_len;
    char first;        /* first value char: '{', '"', digit, '-', 't'... */
} top_member_t;

/* Walk the members of the top-level object. Input must already be validated
 * by cJSON (structure is trusted); returns -1 only on defensive surprises.
 * Collects at most CT_PROV_MAX_TOP_MEMBERS entries; surplus members are
 * skipped (protocol envelopes have at most five). */
static int scan_top_members(const char *json, size_t len,
                            top_member_t *members, int cap)
{
    size_t i = 0;
    if (i >= len || json[i] != '{') {
        return -1;
    }
    i++;
    int count = 0;
    while (i < len) {
        while (i < len && is_json_ws(json[i])) {
            i++;
        }
        if (i >= len) {
            return -1;
        }
        if (json[i] == '}') {
            return count;
        }
        if (count > 0) {
            if (json[i] != ',') {
                return -1;
            }
            i++;
            while (i < len && is_json_ws(json[i])) {
                i++;
            }
        }
        if (i >= len || json[i] != '"') {
            return -1;
        }
        const size_t key_start = i + 1;
        if (!skip_string(json, len, &i)) {
            return -1;
        }
        const size_t key_end = i - 1;
        while (i < len && is_json_ws(json[i])) {
            i++;
        }
        if (i >= len || json[i] != ':') {
            return -1;
        }
        i++;
        while (i < len && is_json_ws(json[i])) {
            i++;
        }
        if (i >= len) {
            return -1;
        }
        const size_t val_start = i;
        const char first = json[i];
        if (first == '"') {
            if (!skip_string(json, len, &i)) {
                return -1;
            }
        } else if (first == '{' || first == '[') {
            /* Balanced skip over the nested value. */
            const char open = first;
            const char close = (first == '{') ? '}' : ']';
            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            while (i < len) {
                const char c = json[i];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    i++;
                    continue;
                }
                if (c == '"') {
                    in_string = true;
                } else if (c == open) {
                    depth++;
                } else if (c == close) {
                    depth--;
                    if (depth == 0) {
                        i++;
                        break;
                    }
                }
                i++;
            }
            if (depth != 0) {
                return -1;
            }
        } else {
            /* Number or literal: run to the next delimiter. */
            while (i < len && json[i] != ',' && json[i] != '}' &&
                   !is_json_ws(json[i])) {
                i++;
            }
        }
        if (count < cap) {
            members[count].key = json + key_start;
            members[count].key_len = key_end - key_start;
            members[count].val = json + val_start;
            members[count].val_len = i - val_start;
            members[count].first = first;
            count++;
        }
    }
    return -1;
}

static const top_member_t *member_by_key(const top_member_t *members,
                                         int count, const char *key)
{
    const size_t key_len = strlen(key);
    for (int i = 0; i < count; i++) {
        if (members[i].key_len == key_len &&
            memcmp(members[i].key, key, key_len) == 0) {
            return &members[i];
        }
    }
    return NULL;
}

static char *dup_slice(const char *s, size_t len)
{
    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

static char *dup_cjson_string(const cJSON *item)
{
    const char *s = cJSON_GetStringValue(item);
    if (s == NULL) {
        return NULL;
    }
    char *copy = malloc(strlen(s) + 1);
    if (copy != NULL) {
        strcpy(copy, s);
    }
    return copy;
}

/* Parse an exact int64 out of a raw number token. Returns false when the
 * token uses float notation (".", "e", "E") or overflows int64 — both are
 * "integer written as float" under the Dart decoder's semantics. */
static bool parse_int_token(const top_member_t *m, int64_t *out)
{
    if (m->first != '-' && (m->first < '0' || m->first > '9')) {
        return false; /* not a number at all: shape problem, not float */
    }
    for (size_t i = 0; i < m->val_len; i++) {
        const char c = m->val[i];
        if (c == '.' || c == 'e' || c == 'E') {
            return false;
        }
    }
    char token[32];
    const size_t n =
        m->val_len < sizeof(token) - 1 ? m->val_len : sizeof(token) - 1;
    memcpy(token, m->val, n);
    token[n] = '\0';
    errno = 0;
    char *end = NULL;
    const long long v = strtoll(token, &end, 10);
    if (errno == ERANGE || end == token || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* ---- decode ----------------------------------------------------------- */

static int reject(ct_prov_envelope_error_t reason,
                  ct_prov_envelope_error_t *err)
{
    if (err != NULL) {
        *err = reason;
    }
    return -1;
}

int ct_prov_envelope_decode(const uint8_t *bytes, size_t len,
                            ct_prov_envelope_t *out,
                            ct_prov_envelope_error_t *err)
{
    if (bytes == NULL || out == NULL) {
        return reject(CT_PROV_ENV_ERR_MALFORMED, err);
    }

    /* BOM must be caught at the byte level: cJSON (like dart:convert)
     * silently skips a leading BOM. */
    if (len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
        bytes[2] == 0xBF) {
        return reject(CT_PROV_ENV_ERR_BOM, err);
    }
    if (!utf8_valid(bytes, len)) {
        return reject(CT_PROV_ENV_ERR_INVALID_UTF8, err);
    }

    char *text = malloc(len + 1);
    if (text == NULL) {
        return reject(CT_PROV_ENV_ERR_MALFORMED, err);
    }
    memcpy(text, bytes, len);
    text[len] = '\0';

    size_t start = 0;
    while (start < len && is_json_ws(text[start])) {
        start++;
    }
    if (start >= len || text[start] != '{') {
        free(text);
        return reject(CT_PROV_ENV_ERR_NOT_OBJECT, err);
    }
    const size_t end = scan_object_end(text, len, start);
    if (end == (size_t)-1) {
        free(text);
        return reject(CT_PROV_ENV_ERR_MALFORMED, err);
    }
    for (size_t i = end; i < len; i++) {
        if (!is_json_ws(text[i])) {
            free(text);
            return reject(CT_PROV_ENV_ERR_TRAILING_GARBAGE, err);
        }
    }

    /* Whole-document JSON validation and the decoded string fields. */
    cJSON *root = cJSON_Parse(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(text);
        return reject(CT_PROV_ENV_ERR_MALFORMED, err);
    }

    top_member_t members[CT_PROV_MAX_TOP_MEMBERS];
    const int mcount = scan_top_members(text + start, len - start, members,
                                        CT_PROV_MAX_TOP_MEMBERS);
    if (mcount < 0) {
        cJSON_Delete(root);
        free(text);
        return reject(CT_PROV_ENV_ERR_MALFORMED, err);
    }

    /* Integers must not be written as floats: checked before shape rules,
     * exactly like the Dart reference. */
    for (int k = 0; k < 2; k++) {
        const char *key = k == 0 ? "id" : "seq";
        const top_member_t *m = member_by_key(members, mcount, key);
        if (m == NULL || m->first == '"') {
            continue; /* absent or non-numeric: shape rules decide later */
        }
        int64_t dummy;
        if ((m->first == '-' || (m->first >= '0' && m->first <= '9')) &&
            !parse_int_token(m, &dummy)) {
            cJSON_Delete(root);
            free(text);
            return reject(CT_PROV_ENV_ERR_FLOAT_INT, err);
        }
    }

    memset(out, 0, sizeof(*out));
    int rc = -1;
    ct_prov_envelope_error_t why = CT_PROV_ENV_ERR_INVALID_SHAPE;

    const top_member_t *m_event = member_by_key(members, mcount, "event");
    const top_member_t *m_id = member_by_key(members, mcount, "id");
    const top_member_t *m_seq = member_by_key(members, mcount, "seq");
    const top_member_t *m_data = member_by_key(members, mcount, "data");
    const top_member_t *m_ok = member_by_key(members, mcount, "ok");
    const top_member_t *m_err = member_by_key(members, mcount, "error");
    const cJSON *j_op = cJSON_GetObjectItem(root, "op");

    if (m_event != NULL) {
        /* Event envelope: {seq, event, data}. */
        const cJSON *j_event = cJSON_GetObjectItem(root, "event");
        if (m_seq == NULL || !parse_int_token(m_seq, &out->seq) ||
            out->seq < 0 || !cJSON_IsString(j_event) || m_data == NULL ||
            m_data->first != '{') {
            goto done;
        }
        out->kind = CT_PROV_ENV_EVENT;
        out->event = dup_cjson_string(j_event);
        out->data_json = dup_slice(m_data->val, m_data->val_len);
        if (out->event == NULL || out->data_json == NULL) {
            ct_prov_envelope_free(out);
            why = CT_PROV_ENV_ERR_MALFORMED;
            goto done;
        }
    } else {
        if (m_id == NULL || !parse_int_token(m_id, &out->id) ||
            out->id < 0 || !cJSON_IsString(j_op) ||
            j_op->valuestring == NULL || j_op->valuestring[0] == '\0') {
            goto done;
        }
        if (m_ok != NULL) {
            /* Response envelope: {id, op, ok, data|error}. */
            const cJSON *j_ok = cJSON_GetObjectItem(root, "ok");
            const bool is_true = cJSON_IsTrue(j_ok);
            const bool is_false = cJSON_IsFalse(j_ok);
            if (!is_true && !is_false) {
                goto done;
            }
            const size_t op_len = strlen(j_op->valuestring);
            if (op_len < 7 ||
                strcmp(j_op->valuestring + op_len - 7, ".result") != 0) {
                goto done;
            }
            if (is_true) {
                if (m_data != NULL && m_data->first != '{') {
                    goto done;
                }
                out->kind = CT_PROV_ENV_RESPONSE;
                out->ok = true;
                out->op = dup_cjson_string(j_op);
                out->data_json = m_data != NULL
                                     ? dup_slice(m_data->val, m_data->val_len)
                                     : NULL;
                if (out->op == NULL ||
                    (m_data != NULL && out->data_json == NULL)) {
                    ct_prov_envelope_free(out);
                    why = CT_PROV_ENV_ERR_MALFORMED;
                    goto done;
                }
            } else {
                if (m_err == NULL || m_err->first != '{') {
                    goto done;
                }
                char *err_text = dup_slice(m_err->val, m_err->val_len);
                if (err_text == NULL) {
                    why = CT_PROV_ENV_ERR_MALFORMED;
                    goto done;
                }
                cJSON *j_err = cJSON_Parse(err_text);
                free(err_text);
                if (j_err == NULL) {
                    goto done;
                }
                const cJSON *j_code = cJSON_GetObjectItem(j_err, "code");
                const cJSON *j_msg = cJSON_GetObjectItem(j_err, "message");
                if (!cJSON_IsString(j_code) || !cJSON_IsString(j_msg)) {
                    cJSON_Delete(j_err);
                    goto done; /* error object shape: invalid_shape */
                }
                out->kind = CT_PROV_ENV_RESPONSE;
                out->ok = false;
                out->op = dup_cjson_string(j_op);
                out->error_code = dup_cjson_string(j_code);
                out->error_message = dup_cjson_string(j_msg);
                cJSON_Delete(j_err);
                if (out->op == NULL || out->error_code == NULL ||
                    out->error_message == NULL) {
                    ct_prov_envelope_free(out);
                    why = CT_PROV_ENV_ERR_MALFORMED;
                    goto done;
                }
            }
        } else {
            /* Request envelope: {id, op, data}. */
            if (m_data == NULL || m_data->first != '{') {
                goto done;
            }
            out->kind = CT_PROV_ENV_REQUEST;
            out->op = dup_cjson_string(j_op);
            out->data_json = dup_slice(m_data->val, m_data->val_len);
            if (out->op == NULL || out->data_json == NULL) {
                ct_prov_envelope_free(out);
                why = CT_PROV_ENV_ERR_MALFORMED;
                goto done;
            }
        }
    }

    rc = 0;

done:
    cJSON_Delete(root);
    free(text);
    if (rc != 0 && err != NULL) {
        *err = why;
    }
    return rc;
}

/* ---- encode ----------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    bool overflow;
} emit_t;

static void emit_raw(emit_t *e, const char *s, size_t len)
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

static void emit_str(emit_t *e, const char *s)
{
    emit_raw(e, s, strlen(s));
}

static void emit_i64(emit_t *e, int64_t v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    emit_str(e, tmp);
}

/* Escape a string per canonical JSON: quote, backslash, and control chars;
 * UTF-8 bytes pass through verbatim (same policy as the Dart encoder). */
static void emit_json_string(emit_t *e, const char *s)
{
    emit_str(e, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        const unsigned char c = *p;
        char esc[7];
        switch (c) {
        case '"':
            emit_str(e, "\\\"");
            continue;
        case '\\':
            emit_str(e, "\\\\");
            continue;
        case '\b':
            emit_str(e, "\\b");
            continue;
        case '\f':
            emit_str(e, "\\f");
            continue;
        case '\n':
            emit_str(e, "\\n");
            continue;
        case '\r':
            emit_str(e, "\\r");
            continue;
        case '\t':
            emit_str(e, "\\t");
            continue;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                emit_str(e, esc);
            } else {
                emit_raw(e, (const char *)&c, 1);
            }
        }
    }
    emit_str(e, "\"");
}

static void emit_data(emit_t *e, const char *data_json)
{
    if (data_json == NULL || data_json[0] == '\0') {
        emit_str(e, "{}");
    } else {
        emit_str(e, data_json);
    }
}

size_t ct_prov_envelope_encode_request(int64_t id, const char *op,
                                       const char *data_json, char *buf,
                                       size_t cap)
{
    if (buf == NULL || cap == 0 || op == NULL) {
        return 0;
    }
    emit_t e = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    emit_str(&e, "{\"id\":");
    emit_i64(&e, id);
    emit_str(&e, ",\"op\":");
    emit_json_string(&e, op);
    emit_str(&e, ",\"data\":");
    emit_data(&e, data_json);
    emit_str(&e, "}");
    if (e.overflow) {
        return 0;
    }
    buf[e.pos] = '\0';
    return e.pos;
}

size_t ct_prov_envelope_encode_response(int64_t id, const char *op, bool ok,
                                        const char *data_json,
                                        const char *error_code,
                                        const char *error_message, char *buf,
                                        size_t cap)
{
    if (buf == NULL || cap == 0 || op == NULL) {
        return 0;
    }
    if (ok && (error_code != NULL || error_message != NULL)) {
        return 0; /* error must be absent when ok */
    }
    if (!ok && (error_code == NULL || error_message == NULL)) {
        return 0; /* error is required when not ok */
    }
    emit_t e = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    emit_str(&e, "{\"id\":");
    emit_i64(&e, id);
    emit_str(&e, ",\"op\":");
    emit_json_string(&e, op);
    if (ok) {
        emit_str(&e, ",\"ok\":true,\"data\":");
        emit_data(&e, data_json);
    } else {
        emit_str(&e, ",\"ok\":false,\"error\":{\"code\":");
        emit_json_string(&e, error_code);
        emit_str(&e, ",\"message\":");
        emit_json_string(&e, error_message);
        emit_str(&e, "}");
    }
    emit_str(&e, "}");
    if (e.overflow) {
        return 0;
    }
    buf[e.pos] = '\0';
    return e.pos;
}

size_t ct_prov_envelope_encode_event(int64_t seq, const char *event,
                                     const char *data_json, char *buf,
                                     size_t cap)
{
    if (buf == NULL || cap == 0 || event == NULL) {
        return 0;
    }
    emit_t e = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    emit_str(&e, "{\"seq\":");
    emit_i64(&e, seq);
    emit_str(&e, ",\"event\":");
    emit_json_string(&e, event);
    emit_str(&e, ",\"data\":");
    emit_data(&e, data_json);
    emit_str(&e, "}");
    if (e.overflow) {
        return 0;
    }
    buf[e.pos] = '\0';
    return e.pos;
}
