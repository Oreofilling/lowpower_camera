/*
 * CT-Prov/1 config.set (protocol doc section 10, CT_CONFIG_SCHEMA.md).
 * Two passes over the parsed request: validate every submitted section and
 * field first (all-or-nothing), then write through the injected store.
 * Within a submitted section the field set is replaced wholesale, so
 * declared-but-omitted optional fields are written back at their schema
 * default (doc 10: no field-level merge).
 */
#include "ct_prov_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ct_prov_envelope.h"
#include "ct_prov_json.h"

#define CT_PROV_SET_RESULT_OP "config.set.result"

/* Field storage kinds. */
#define ST_STR 0
#define ST_U8 1
#define ST_U32 2

/* Field flags. */
#define F_PASSWORD 0x01  /* masked by config.get; never echoed in clear */
#define F_REQUIRED 0x02  /* must be present when the section is submitted */

typedef enum {
    FLD_STR,   /* string with [min_len, max_len] byte bounds */
    FLD_BOOL,  /* JSON boolean, stored as u8 0/1 */
    FLD_INT,   /* JSON integer in [min_val, max_val] */
    FLD_ENUM,  /* string out of enum_vals, stored as its index (u8) */
} fld_type_t;

typedef struct {
    const char *name;
    uint8_t type;      /* fld_type_t */
    uint8_t store_kind; /* ST_* */
    uint8_t flags;
    size_t min_len, max_len;
    int64_t min_val, max_val;
    const char *key;   /* NVS key, CT_CONFIG_SCHEMA.md 10.1 */
    const char *def_str;
    uint32_t def_num;
    const char *const *enum_vals; /* FLD_ENUM only, NULL-terminated */
    bool (*check)(const char *value, size_t len); /* extra format rule */
} field_spec_t;

typedef struct {
    const char *name;
    const field_spec_t *fields;
    size_t field_count;
} section_spec_t;

/* ---- per-field format rules ------------------------------------------ */

/* Wi-Fi password: empty (open network) or 8..63 bytes (schema 3). */
static bool chk_wifi_password(const char *value, size_t len)
{
    (void)value;
    return len == 0 || len >= 8;
}

/* SIM PIN: 4..8 digits (schema 4). */
static bool chk_digits(const char *value, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

/* MQTT topic: no wildcard characters (schema 8). */
static bool chk_topic(const char *value, size_t len)
{
    (void)len;
    return strpbrk(value, "+#") == NULL;
}

/* Webhook URL: absolute http/https URI (schema 9). */
static bool chk_url(const char *value, size_t len)
{
    if (len < 8) { /* "http://x" */
        return false;
    }
    return strncmp(value, "http://", 7) == 0 ||
           strncmp(value, "https://", 8) == 0;
}

/* Extra header: "Key: Value" shape (schema 9). */
static bool chk_header(const char *value, size_t len)
{
    (void)len;
    return value[0] == '\0' || strchr(value, ':') != NULL;
}

/* ---- NE101 section tables (schema 2-4, 8, 9) -------------------------- */

static const char *const k_auth_values[] = {"none", "pap", "chap",
                                            "pap_or_chap", NULL};

#define FIELDS_END {NULL, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL, NULL}

static const field_spec_t k_wifi_fields[] = {
    {"ssid", FLD_STR, ST_STR, F_REQUIRED, 1, 31, 0, 0, "wifi:ssid", "", 0,
     NULL, NULL},
    {"password", FLD_STR, ST_STR, F_PASSWORD, 0, 63, 0, 0, "wifi:password",
     "", 0, NULL, chk_wifi_password},
    FIELDS_END,
};

static const field_spec_t k_cellular_fields[] = {
    {"apn", FLD_STR, ST_STR, F_REQUIRED, 1, 31, 0, 0, "cat1:apn", "", 0,
     NULL, NULL},
    {"username", FLD_STR, ST_STR, 0, 0, 63, 0, 0, "cat1:user", "", 0, NULL,
     NULL},
    {"password", FLD_STR, ST_STR, F_PASSWORD, 0, 63, 0, 0, "cat1:password",
     "", 0, NULL, NULL},
    {"pin", FLD_STR, ST_STR, F_PASSWORD, 4, 8, 0, 0, "cat1:pin", "", 0, NULL,
     chk_digits},
    {"auth", FLD_ENUM, ST_U8, 0, 0, 0, 0, 0, "cat1:authType", NULL, 0,
     k_auth_values, NULL},
    FIELDS_END,
};

static const field_spec_t k_mqtt_fields[] = {
    {"enable", FLD_BOOL, ST_U8, F_REQUIRED, 0, 0, 0, 0, "mqtt:enable", NULL,
     0, NULL, NULL},
    {"host", FLD_STR, ST_STR, F_REQUIRED, 1, 127, 0, 0, "mqtt:host", "", 0,
     NULL, NULL},
    {"port", FLD_INT, ST_U32, F_REQUIRED, 0, 0, 1, 65535, "mqtt:port", NULL,
     1883, NULL, NULL},
    {"client_id", FLD_STR, ST_STR, 0, 0, 127, 0, 0, "mqtt:clientId", "", 0,
     NULL, NULL},
    {"username", FLD_STR, ST_STR, 0, 0, 63, 0, 0, "mqtt:user", "", 0, NULL,
     NULL},
    {"password", FLD_STR, ST_STR, F_PASSWORD, 0, 63, 0, 0, "mqtt:password",
     "", 0, NULL, NULL},
    {"tls", FLD_BOOL, ST_U8, 0, 0, 0, 0, 0, "mqtt:tlsEnable", NULL, 0, NULL,
     NULL},
    {"topic", FLD_STR, ST_STR, 0, 0, 127, 0, 0, "mqtt:topic", "", 0, NULL,
     chk_topic},
    {"qos", FLD_INT, ST_U8, 0, 0, 0, 0, 2, "mqtt:qos", NULL, 0, NULL, NULL},
    FIELDS_END,
};

static const field_spec_t k_webhook_fields[] = {
    {"enable", FLD_BOOL, ST_U8, F_REQUIRED, 0, 0, 0, 0, "push:mode", NULL,
     0, NULL, NULL},
    {"url", FLD_STR, ST_STR, F_REQUIRED, 1, 255, 0, 0, "whk:url", "", 0,
     NULL, chk_url},
    {"header", FLD_STR, ST_STR, 0, 0, 255, 0, 0, "whk:header", "", 0, NULL,
     chk_header},
    FIELDS_END,
};

static const section_spec_t k_sections[] = {
    {"wifi", k_wifi_fields, sizeof(k_wifi_fields) / sizeof(k_wifi_fields[0])},
    {"cellular", k_cellular_fields,
     sizeof(k_cellular_fields) / sizeof(k_cellular_fields[0])},
    {"mqtt", k_mqtt_fields, sizeof(k_mqtt_fields) / sizeof(k_mqtt_fields[0])},
    {"webhook", k_webhook_fields,
     sizeof(k_webhook_fields) / sizeof(k_webhook_fields[0])},
};

/* ---- helpers ----------------------------------------------------------- */

static size_t error_response(int64_t id, const char *code, const char *message,
                             char *resp, size_t resp_cap)
{
    return ct_prov_envelope_encode_response(id, CT_PROV_SET_RESULT_OP, false,
                                            NULL, code, message, resp,
                                            resp_cap);
}

static const section_spec_t *find_section(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_sections) / sizeof(k_sections[0]); i++) {
        if (strcmp(k_sections[i].name, name) == 0) {
            return &k_sections[i];
        }
    }
    return NULL;
}

static const field_spec_t *find_field(const section_spec_t *sec,
                                      const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sec->field_count; i++) {
        const field_spec_t *f = &sec->fields[i];
        if (f->name != NULL && strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return NULL;
}

/* cJSON keeps valueint truncated; a non-integral double must be rejected
 * the same way the envelope layer rejects floats written for ids. */
static bool json_is_integral(const cJSON *item, int64_t *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    if ((double)item->valueint != item->valuedouble) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static size_t field_error(int64_t id, const char *code, const char *section,
                          const char *field, const char *detail, char *resp,
                          size_t resp_cap)
{
    char msg[112];
    snprintf(msg, sizeof(msg), "%.24s.%.24s: %.48s", section, field, detail);
    return error_response(id, code, msg, resp, resp_cap);
}

/* Pass 1: validate one submitted section; returns true when acceptable. */
static bool validate_section(int64_t id, const cJSON *sec,
                             const section_spec_t *spec, char *resp,
                             size_t resp_cap, size_t *answer)
{
    if (!cJSON_IsObject(sec)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "section %.24s must be an object",
                 spec->name);
        *answer = error_response(id, "INVALID_ARGUMENT", msg, resp, resp_cap);
        return false;
    }

    /* Unknown fields are refused even when their value would validate. */
    for (const cJSON *fld = sec->child; fld != NULL; fld = fld->next) {
        if (find_field(spec, fld->string) == NULL) {
            *answer = field_error(id, "FIELD_NOT_SUPPORTED", spec->name,
                                  fld->string != NULL ? fld->string : "?",
                                  "field not declared", resp, resp_cap);
            return false;
        }
    }

    for (size_t i = 0; i < spec->field_count; i++) {
        const field_spec_t *f = &spec->fields[i];
        if (f->name == NULL) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItem(sec, f->name);
        if (item == NULL) {
            if ((f->flags & F_REQUIRED) != 0) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "required field missing",
                                      resp, resp_cap);
                return false;
            }
            continue;
        }

        switch (f->type) {
        case FLD_STR: {
            const char *v = cJSON_GetStringValue(item);
            if (v == NULL) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "must be a string", resp,
                                      resp_cap);
                return false;
            }
            const size_t len = strlen(v);
            if (len < f->min_len || len > f->max_len) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "length out of range", resp,
                                      resp_cap);
                return false;
            }
            if (f->check != NULL && !f->check(v, len)) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "format not accepted", resp,
                                      resp_cap);
                return false;
            }
            break;
        }
        case FLD_BOOL:
            if (!cJSON_IsBool(item)) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "must be a boolean", resp,
                                      resp_cap);
                return false;
            }
            break;
        case FLD_INT: {
            int64_t v;
            if (!json_is_integral(item, &v)) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "must be an integer", resp,
                                      resp_cap);
                return false;
            }
            if (v < f->min_val || v > f->max_val) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "value out of range", resp,
                                      resp_cap);
                return false;
            }
            break;
        }
        case FLD_ENUM: {
            const char *v = cJSON_GetStringValue(item);
            bool matched = false;
            if (v != NULL) {
                for (size_t k = 0; f->enum_vals[k] != NULL; k++) {
                    if (strcmp(f->enum_vals[k], v) == 0) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                *answer = field_error(id, "INVALID_ARGUMENT", spec->name,
                                      f->name, "value not in enum", resp,
                                      resp_cap);
                return false;
            }
            break;
        }
        default:
            *answer = error_response(id, "INTERNAL_ERROR", "bad field spec",
                                     resp, resp_cap);
            return false;
        }
    }
    return true;
}

/* Pass 2: write one section (submitted values plus schema defaults for the
 * omitted optional fields). Returns 0 on success. */
static int write_section(const ct_prov_store_t *store, const cJSON *sec,
                         const section_spec_t *spec)
{
    for (size_t i = 0; i < spec->field_count; i++) {
        const field_spec_t *f = &spec->fields[i];
        if (f->name == NULL) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItem(sec, f->name);

        switch (f->store_kind) {
        case ST_STR: {
            const char *v = item != NULL ? cJSON_GetStringValue(item)
                                         : f->def_str;
            if (v == NULL) {
                return -1;
            }
            if (store->write_str(store->ctx, f->key, v) != 0) {
                return -1;
            }
            break;
        }
        case ST_U8: {
            uint8_t v;
            if (item == NULL) {
                v = (uint8_t)f->def_num;
            } else if (f->type == FLD_BOOL) {
                v = cJSON_IsTrue(item) ? 1 : 0;
            } else if (f->type == FLD_ENUM) {
                const char *s = cJSON_GetStringValue(item);
                v = 0;
                if (s != NULL) {
                    for (size_t k = 0; f->enum_vals[k] != NULL; k++) {
                        if (strcmp(f->enum_vals[k], s) == 0) {
                            v = (uint8_t)k;
                            break;
                        }
                    }
                }
            } else { /* FLD_INT already range-validated to fit u8 */
                v = (uint8_t)item->valueint;
            }
            if (store->write_u8(store->ctx, f->key, v) != 0) {
                return -1;
            }
            break;
        }
        case ST_U32: {
            const uint32_t v =
                item != NULL ? (uint32_t)item->valueint : f->def_num;
            if (store->write_u32(store->ctx, f->key, v) != 0) {
                return -1;
            }
            break;
        }
        default:
            return -1;
        }
    }
    return 0;
}

size_t ct_prov_config_set_response(int64_t id, const char *data_json,
                                   const ct_prov_store_t *store,
                                   char *scratch, size_t scratch_cap,
                                   char *resp, size_t resp_cap)
{
    if (store == NULL || store->write_str == NULL || store->write_u8 == NULL ||
        store->write_u32 == NULL) {
        return error_response(id, "INTERNAL_ERROR", "no config store wired",
                              resp, resp_cap);
    }

    cJSON *root = cJSON_Parse(data_json != NULL ? data_json : "{}");
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return error_response(id, "INVALID_ARGUMENT",
                              "data must be an object", resp, resp_cap);
    }

    /* Pass 1 over every submitted section; nothing is written until all of
     * them validate (doc 10 all-or-nothing). */
    for (const cJSON *sec = root->child; sec != NULL; sec = sec->next) {
        const section_spec_t *spec = find_section(sec->string);
        if (spec == NULL) {
            char msg[64];
            snprintf(msg, sizeof(msg), "section not declared: %.32s",
                     sec->string != NULL ? sec->string : "?");
            cJSON_Delete(root);
            return error_response(id, "SECTION_NOT_SUPPORTED", msg, resp,
                                  resp_cap);
        }
        size_t answer = 0;
        if (!validate_section(id, sec, spec, resp, resp_cap, &answer)) {
            cJSON_Delete(root);
            return answer;
        }
    }

    /* Pass 2: persist. A storage failure mid-way is reported as
     * STORAGE_FAILED; validation already guaranteed no partial write for
     * malformed payloads. */
    for (const cJSON *sec = root->child; sec != NULL; sec = sec->next) {
        const section_spec_t *spec = find_section(sec->string);
        if (write_section(store, sec, spec) != 0) {
            cJSON_Delete(root);
            return error_response(id, "STORAGE_FAILED",
                                  "persistent write failed", resp, resp_cap);
        }
    }

    /* stored[] lists the submitted sections in request order. */
    ct_emit_t e = {.buf = scratch, .cap = scratch_cap, .pos = 0,
                   .overflow = false};
    ct_emit_str(&e, "{\"stored\":[");
    bool first = true;
    for (const cJSON *sec = root->child; sec != NULL; sec = sec->next) {
        if (!first) {
            ct_emit_str(&e, ",");
        }
        first = false;
        ct_emit_json_string(&e, sec->string != NULL ? sec->string : "?");
    }
    ct_emit_str(&e, "],\"apply\":\"reconnect\"}");
    cJSON_Delete(root);

    if (e.overflow) {
        return error_response(id, "INTERNAL_ERROR",
                              "result payload overflows buffer", resp,
                              resp_cap);
    }
    scratch[e.pos] = '\0';
    return ct_prov_envelope_encode_response(id, CT_PROV_SET_RESULT_OP, true,
                                            scratch, NULL, NULL, resp,
                                            resp_cap);
}
