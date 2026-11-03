/*
 * CT-Prov/1 capabilities and config.get (protocol doc sections 8 and 10,
 * CT_CONFIG_SCHEMA.md). NE101 shape: wifi/cellular/mqtt/webhook, no comm
 * section, wifi without bssid, cellular without roaming, webhook without
 * secret (doc 8 rules + schema 10.1 mapping).
 */
#include "ct_prov_config.h"

#include <string.h>

#include "ct_prov_envelope.h"
#include "ct_prov_json.h"

#define CT_PROV_CAPS_RESULT_OP "communication.capabilities.get.result"
#define CT_PROV_GET_RESULT_OP "config.get.result"

/* Longest value read back per field; every schema string fits. */
#define CT_PROV_VALUE_MAX 256

/* Password-class fields never leave the device in clear (doc 10). */
#define CT_PROV_MASK "********"

static size_t error_response(int64_t id, const char *result_op,
                             const char *code, const char *message,
                             char *resp, size_t resp_cap)
{
    return ct_prov_envelope_encode_response(id, result_op, false, NULL, code,
                                            message, resp, resp_cap);
}

/* Append one "name":<string|number|bool> member with leading comma control.
 * value == NULL emits a JSON null. */
static void emit_member_str(ct_emit_t *e, bool *first, const char *name,
                            const char *value)
{
    if (!*first) {
        ct_emit_str(e, ",");
    }
    *first = false;
    ct_emit_json_string(e, name);
    ct_emit_str(e, ":");
    if (value == NULL) {
        ct_emit_str(e, "null");
    } else {
        ct_emit_json_string(e, value);
    }
}

static void emit_member_u32(ct_emit_t *e, bool *first, const char *name,
                            uint32_t value)
{
    if (!*first) {
        ct_emit_str(e, ",");
    }
    *first = false;
    ct_emit_json_string(e, name);
    ct_emit_str(e, ":");
    ct_emit_i64(e, (int64_t)value);
}

static void emit_member_bool(ct_emit_t *e, bool *first, const char *name,
                             bool value)
{
    if (!*first) {
        ct_emit_str(e, ",");
    }
    *first = false;
    ct_emit_json_string(e, name);
    ct_emit_str(e, value ? ":true" : ":false");
}

static void emit_section_row(ct_emit_t *e, bool *first, const char *name,
                             const char *kind, bool configurable,
                             bool scannable, const char *const *fields)
{
    if (!*first) {
        ct_emit_str(e, ",");
    }
    *first = false;
    ct_emit_str(e, "{\"name\":");
    ct_emit_json_string(e, name);
    ct_emit_str(e, ",\"kind\":");
    ct_emit_json_string(e, kind);
    ct_emit_str(e, configurable ? ",\"configurable\":true"
                                : ",\"configurable\":false");
    ct_emit_str(e, scannable ? ",\"scannable\":true" : ",\"scannable\":false");
    ct_emit_str(e, ",\"fields\":[");
    for (size_t i = 0; fields[i] != NULL; i++) {
        if (i > 0) {
            ct_emit_str(e, ",");
        }
        ct_emit_json_string(e, fields[i]);
    }
    ct_emit_str(e, "]}");
}

size_t ct_prov_capabilities_response(int64_t id, bool wifi_scannable,
                                     const char *current_network,
                                     char *scratch, size_t scratch_cap,
                                     char *resp, size_t resp_cap)
{
    static const char *const k_wifi[] = {"ssid", "password", NULL};
    static const char *const k_cellular[] = {"apn", "username", "password",
                                             "pin", "auth", NULL};
    static const char *const k_mqtt[] = {"enable",
                                         "host",
                                         "port",
                                         "client_id",
                                         "username",
                                         "password",
                                         "tls",
                                         "topic",
                                         "qos",
                                         NULL};
    static const char *const k_webhook[] = {"enable", "url", "header", NULL};

    ct_emit_t e = {.buf = scratch, .cap = scratch_cap, .pos = 0,
                   .overflow = false};
    ct_emit_str(&e, "{\"sections\":[");
    bool first = true;
    emit_section_row(&e, &first, "wifi", "network", true, wifi_scannable,
                     k_wifi);
    emit_section_row(&e, &first, "cellular", "network", true, false,
                     k_cellular);
    emit_section_row(&e, &first, "mqtt", "service", true, false, k_mqtt);
    emit_section_row(&e, &first, "webhook", "service", true, false, k_webhook);
    ct_emit_str(&e, "],\"current\":{\"network\":");
    if (current_network == NULL) {
        ct_emit_str(&e, "null");
    } else {
        ct_emit_json_string(&e, current_network);
    }
    ct_emit_str(&e, "}}");

    if (e.overflow) {
        return error_response(id, CT_PROV_CAPS_RESULT_OP, "INTERNAL_ERROR",
                              "capabilities payload overflows buffer", resp,
                              resp_cap);
    }
    scratch[e.pos] = '\0';
    return ct_prov_envelope_encode_response(id, CT_PROV_CAPS_RESULT_OP, true,
                                            scratch, NULL, NULL, resp,
                                            resp_cap);
}

/*
 * config.get walks each section through the reader and emits only the
 * sections that carry a stored identity (wifi by ssid, cellular by apn,
 * mqtt by host, webhook by url). Password-class values come back masked;
 * unset optional strings are omitted entirely (doc 10 read-back shape).
 */
size_t ct_prov_config_get_response(int64_t id, const ct_prov_reader_t *reader,
                                   char *scratch, size_t scratch_cap,
                                   char *resp, size_t resp_cap)
{
    if (reader == NULL || reader->read_str == NULL ||
        reader->read_u8 == NULL || reader->read_u32 == NULL) {
        return error_response(id, CT_PROV_GET_RESULT_OP, "INTERNAL_ERROR",
                              "no config reader wired", resp, resp_cap);
    }

    ct_emit_t e = {.buf = scratch, .cap = scratch_cap, .pos = 0,
                   .overflow = false};
    ct_emit_str(&e, "{");
    bool section_first = true;
    char val[CT_PROV_VALUE_MAX];
    uint8_t u8v = 0;
    uint32_t u32v = 0;

    reader->read_str(reader->ctx, "wifi:ssid", val, sizeof(val));
    if (val[0] != '\0') {
        bool first = true;
        ct_emit_str(&e, "\"wifi\":{");
        emit_member_str(&e, &first, "ssid", val);
        reader->read_str(reader->ctx, "wifi:password", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "password", CT_PROV_MASK);
        }
        ct_emit_str(&e, "}");
        section_first = false;
    }

    reader->read_str(reader->ctx, "cat1:apn", val, sizeof(val));
    if (val[0] != '\0') {
        bool first = true;
        if (!section_first) {
            ct_emit_str(&e, ",");
        }
        section_first = false;
        ct_emit_str(&e, "\"cellular\":{");
        emit_member_str(&e, &first, "apn", val);
        reader->read_str(reader->ctx, "cat1:user", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "username", val);
        }
        reader->read_str(reader->ctx, "cat1:password", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "password", CT_PROV_MASK);
        }
        reader->read_str(reader->ctx, "cat1:pin", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "pin", CT_PROV_MASK);
        }
        if (reader->read_u8(reader->ctx, "cat1:authType", &u8v) && u8v != 0) {
            static const char *const k_auth[] = {"none", "pap", "chap",
                                                 "pap_or_chap"};
            emit_member_str(&e, &first, "auth", k_auth[u8v % 4]);
        }
        ct_emit_str(&e, "}");
    }

    reader->read_str(reader->ctx, "mqtt:host", val, sizeof(val));
    if (val[0] != '\0') {
        bool first = true;
        if (!section_first) {
            ct_emit_str(&e, ",");
        }
        section_first = false;
        ct_emit_str(&e, "\"mqtt\":{");
        emit_member_bool(&e, &first, "enable",
                         reader->read_u8(reader->ctx, "mqtt:enable", &u8v) &&
                             u8v != 0);
        emit_member_str(&e, &first, "host", val);
        if (reader->read_u32(reader->ctx, "mqtt:port", &u32v)) {
            emit_member_u32(&e, &first, "port", u32v);
        }
        reader->read_str(reader->ctx, "mqtt:clientId", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "client_id", val);
        }
        reader->read_str(reader->ctx, "mqtt:user", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "username", val);
        }
        reader->read_str(reader->ctx, "mqtt:password", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "password", CT_PROV_MASK);
        }
        emit_member_bool(&e, &first, "tls",
                         reader->read_u8(reader->ctx, "mqtt:tlsEnable",
                                         &u8v) &&
                             u8v != 0);
        reader->read_str(reader->ctx, "mqtt:topic", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "topic", val);
        }
        if (reader->read_u8(reader->ctx, "mqtt:qos", &u8v)) {
            emit_member_u32(&e, &first, "qos", u8v);
        }
        ct_emit_str(&e, "}");
    }

    reader->read_str(reader->ctx, "whk:url", val, sizeof(val));
    if (val[0] != '\0') {
        bool first = true;
        if (!section_first) {
            ct_emit_str(&e, ",");
        }
        section_first = false;
        ct_emit_str(&e, "\"webhook\":{");
        emit_member_bool(&e, &first, "enable",
                         reader->read_u8(reader->ctx, "push:mode", &u8v) &&
                             u8v != 0);
        emit_member_str(&e, &first, "url", val);
        reader->read_str(reader->ctx, "whk:header", val, sizeof(val));
        if (val[0] != '\0') {
            emit_member_str(&e, &first, "header", val);
        }
        ct_emit_str(&e, "}");
    }

    ct_emit_str(&e, "}");

    if (e.overflow) {
        return error_response(id, CT_PROV_GET_RESULT_OP, "INTERNAL_ERROR",
                              "config payload overflows buffer", resp,
                              resp_cap);
    }
    scratch[e.pos] = '\0';
    return ct_prov_envelope_encode_response(id, CT_PROV_GET_RESULT_OP, true,
                                            scratch, NULL, NULL, resp,
                                            resp_cap);
}
