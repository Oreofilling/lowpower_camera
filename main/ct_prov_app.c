/**
 * @file ct_prov_app.c
 * @brief CT-Prov/1 application glue: device identity from factory config
 *        and request dispatch (protocol doc sections 4.2, 7, 12).
 *
 * The GATT server itself lives in components/ct_prov_ble; this file only
 * decides who the device is and how requests are answered. Factory info is
 * read from NVS once at start so the request callback stays off flash
 * (doc 13: callbacks must not block).
 */
#include "ct_prov_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "config.h"
#include "utils.h"
#include "ct_prov_ble.h"
#include "ct_prov_envelope.h"

#define TAG "-->CT_PROV_APP"

/* Longest NVS field is MAX_LEN_32-1 chars; worst case every char escapes
 * to 6 bytes (\u00xx). */
#define CT_PROV_ESC_MAX (MAX_LEN_32 * 6)
#define CT_PROV_JSON_OVERFLOW ((size_t)-1)

static deviceInfo_t s_dev;
static ct_prov_ble_identity_t s_identity;

/*
 * Append value at dst+off as a quoted, JSON-escaped string followed by a
 * raw suffix (e.g. ",\"model\":"). Returns the new offset, or
 * CT_PROV_JSON_OVERFLOW when the escaped form does not fit.
 */
static size_t append_json_string(char *dst, size_t cap, size_t off,
                                 const char *value, const char *suffix)
{
    char esc[CT_PROV_ESC_MAX];
    size_t n = 0;
    for (const char *p = value; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        char rep[7];
        size_t rep_len;
        if (c == '"') {
            rep[0] = '\\';
            rep[1] = '"';
            rep_len = 2;
        } else if (c == '\\') {
            rep[0] = '\\';
            rep[1] = '\\';
            rep_len = 2;
        } else if (c < 0x20) {
            snprintf(rep, sizeof(rep), "\\u%04x", c);
            rep_len = 6;
        } else {
            rep[0] = (char)c;
            rep_len = 1;
        }
        if (n + rep_len > sizeof(esc) - 1) {
            return CT_PROV_JSON_OVERFLOW;
        }
        memcpy(esc + n, rep, rep_len);
        n += rep_len;
    }

    const size_t slen = strlen(suffix);
    if (off + n + slen + 3 > cap) {
        return CT_PROV_JSON_OVERFLOW;
    }
    const int w = snprintf(dst + off, cap - off, "\"%.*s\"%s", (int)n, esc,
                           suffix);
    if (w < 0) {
        return CT_PROV_JSON_OVERFLOW;
    }
    return off + (size_t)w;
}

/*
 * device.info.get data object (doc 7). device_name comes from the BLE
 * identity: its charset is validated (uppercase alnum + dash), so it needs
 * no escaping; the NVS strings do.
 */
static size_t device_info_data(char *data, size_t cap)
{
    size_t off = (size_t)snprintf(data, cap,
                                  "{\"device_name\":\"%s\",\"serial_number\":",
                                  s_identity.device_name);
    if (off >= cap) {
        return 0;
    }
    off = append_json_string(data, cap, off, s_dev.sn, ",\"model\":");
    if (off == CT_PROV_JSON_OVERFLOW) {
        return 0;
    }
    off = append_json_string(data, cap, off, s_dev.model, ",\"firmware\":");
    if (off == CT_PROV_JSON_OVERFLOW) {
        return 0;
    }
    off = append_json_string(data, cap, off, s_dev.softVersion,
                             ",\"power\":[\"battery\"],\"protocol_version\":1}");
    if (off == CT_PROV_JSON_OVERFLOW) {
        return 0;
    }
    return off;
}

static size_t error_response(int64_t id, const char *op, const char *code,
                             const char *message, char *resp, size_t cap)
{
    char op_result[80];
    snprintf(op_result, sizeof(op_result), "%.71s.result", op);
    return ct_prov_envelope_encode_response(id, op_result, false, NULL, code,
                                            message, resp, cap);
}

/*
 * Request handler running in the NimBLE host task (doc 13). Only touches
 * the cached RAM copies of the factory info.
 */
static size_t on_request(void *arg, const ct_prov_envelope_t *req, char *resp,
                         size_t cap)
{
    (void)arg;
    if (strcmp(req->op, "device.info.get") == 0) {
        char data[640];
        const size_t n = device_info_data(data, sizeof(data));
        if (n > 0) {
            return ct_prov_envelope_encode_response(
                req->id, "device.info.get.result", true, data, NULL, NULL,
                resp, cap);
        }
        return error_response(req->id, "device.info.get", "INTERNAL_ERROR",
                              "device info unavailable", resp, cap);
    }
    /* F1 ships device.info.get only; everything else is a typed refusal. */
    return error_response(req->id, req->op, "UNSUPPORTED_OPERATION",
                          "operation not implemented", resp, cap);
}

esp_err_t ct_prov_app_start(void)
{
    deviceInfo_t dev;
    if (cfg_get_device_info(&dev) != ESP_OK) {
        ESP_LOGE(TAG, "factory device info unreadable");
        return ESP_ERR_INVALID_STATE;
    }

    /* Doc 4.2.2: model codes are a fixed registry; 0x00/0xFF forbidden. */
    uint8_t model_code;
    if (strcmp(dev.model, "NE101") == 0) {
        model_code = 0x01;
    } else if (strcmp(dev.model, "NE301") == 0) {
        model_code = 0x02;
    } else {
        ESP_LOGE(TAG, "model '%s' has no CT-Prov model code", dev.model);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!is_valid_mac(dev.mac)) {
        ESP_LOGE(TAG, "MAC '%s' is not a valid XX:XX:XX:XX:XX:XX string",
                 dev.mac);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mac[6];
    mac_str2hex(dev.mac, mac);

    /* Doc 4.2.1: device_name = MODEL + '-' + last 3 MAC bytes in display
     * order. The %.5s precision states the strcmp-verified model length to
     * the compiler (5+1+6+1 fits NAME_MAX with room). ct_prov_ble_start
     * re-validates the full name rules. */
    ct_prov_ble_identity_t identity = {
        .model_code = model_code,
        .mac_suffix = {mac[3], mac[4], mac[5]},
    };
    snprintf(identity.device_name, sizeof(identity.device_name),
             "%.5s-%02X%02X%02X", dev.model, mac[3], mac[4], mac[5]);

    s_dev = dev;
    s_identity = identity;
    return ct_prov_ble_start(&s_identity, on_request, NULL);
}

void ct_prov_app_stop(void)
{
    ct_prov_ble_stop();
}
