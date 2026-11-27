/**
 * @file ct_prov_app.c
 * @brief CT-Prov/1 application glue: device identity from factory config
 *        and request dispatch (protocol doc sections 4.2, 7-10, 12).
 *
 * The GATT server itself lives in components/ct_prov_ble; this file only
 * decides who the device is and how requests are answered. Factory info is
 * read from NVS once at start so the request callback stays off flash
 * (doc 13: callbacks must not block). The F2 operations (capabilities,
 * config.get/set, scan) live in components/ct_prov_config and only reach
 * NVS/Wi-Fi through the adapters below.
 */
#include "ct_prov_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "utils.h"
#include "ct_prov_ble.h"
#include "ct_prov_config.h"
#include "ct_prov_envelope.h"
#include "net_module.h"

#define TAG "-->CT_PROV_APP"

/* Longest NVS field is MAX_LEN_32-1 chars; worst case every char escapes
 * to 6 bytes (\u00xx). */
#define CT_PROV_ESC_MAX (MAX_LEN_32 * 6)
#define CT_PROV_JSON_OVERFLOW ((size_t)-1)

static deviceInfo_t s_dev;
static ct_prov_ble_identity_t s_identity;
static bool s_active;

/* Scratch shared by the F2 handlers: heap-allocated per dispatch so the
 * memory returns to the RTOS heap between provisioning requests (duty
 * cycles never provision). The dispatch callback itself runs on the
 * 4096-byte NimBLE host task stack, hence heap rather than stack. */
#define CT_PROV_SCRATCH_SIZE 3840

/* Serializes dispatch between the NimBLE host task (BLE) and the httpd
 * worker (softAP HTTP). */
static SemaphoreHandle_t s_dispatch_mux;

/* ---- NVS adapters for components/ct_prov_config ------------------------ */

/* cfg_set_* log failures internally and give no status, so the store cannot
 * report one; STORAGE_FAILED stays a component-level guard for stores that
 * can (and for the host tests). */
static int store_str(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    cfg_set_str(key, value);
    return 0;
}

static int store_u8(void *ctx, const char *key, uint8_t value)
{
    (void)ctx;
    cfg_set_u8(key, value);
    return 0;
}

static int store_u32(void *ctx, const char *key, uint32_t value)
{
    (void)ctx;
    cfg_set_u32(key, value);
    return 0;
}

static const ct_prov_store_t s_store = {
    .ctx = NULL, .write_str = store_str, .write_u8 = store_u8,
    .write_u32 = store_u32};

/* Reads fall back to the schema defaults: every u8 key defaults to 0 and
 * the only u32 key is mqtt:port (CT_CONFIG_SCHEMA.md 10.1). A section is
 * only emitted when its identity string is non-empty, so defaulted numerics
 * never surface for an unconfigured section. */
static void read_str(void *ctx, const char *key, char *out, size_t cap)
{
    (void)ctx;
    cfg_get_str(key, out, cap, "");
}

static bool read_u8(void *ctx, const char *key, uint8_t *out)
{
    (void)ctx;
    cfg_get_u8(key, out, 0);
    return true;
}

static bool read_u32(void *ctx, const char *key, uint32_t *out)
{
    (void)ctx;
    cfg_get_u32(key, out, 1883);
    return true;
}

static const ct_prov_reader_t s_reader = {
    .ctx = NULL, .read_str = read_str, .read_u8 = read_u8,
    .read_u32 = read_u32};

/* ---- Wi-Fi scan cache (doc 9) ------------------------------------------- */

#define SCAN_CACHE_MAX 64

/* Both arrays are heap-allocated on the first scan and freed when the
 * provisioning app stops: 64 wifi_ap_record_t alone would blow the host
 * task stack, and duty cycles never scan. */
static ct_prov_scan_item_t *s_scan_cache;
static wifi_ap_record_t *s_scan_raw;
static size_t s_scan_count;

/* Only the radio module reports "wifi" as scannable; on CAT1 hardware the
 * config-mode Wi-Fi runs AP-only and cannot scan. */
static bool wifi_scannable(void)
{
    return !netModule_is_cat1();
}

/* Doc 8 current.network: the section whose identity is configured on the
 * hardware this unit actually carries. NULL when unconfigured. */
static const char *current_network_name(void)
{
    char v[MAX_LEN_32];
    if (netModule_is_cat1()) {
        cfg_get_str(KEY_CAT1_APN, v, sizeof(v), "");
        return v[0] != '\0' ? "cellular" : NULL;
    }
    cfg_get_str(KEY_WIFI_SSID, v, sizeof(v), "");
    return v[0] != '\0' ? "wifi" : NULL;
}

/*
 * Refresh the scan cache: one blocking all-channel scan (~1.5-2 s). This
 * runs inside the NimBLE host task; the BLE controller keeps the link at
 * the link layer while the host blocks, and the client simply waits for
 * the response. SW coexistence shares the airtime between the radios.
 * The cache is only refreshed on cursor-less requests so a cursor walk
 * pages a stable array (doc 9); within one config-mode window an old
 * cursor is either past the end (rejected as stale) or indexes the same
 * SSID space, so the cross-connection invalidation rule is relaxed here.
 * Returns 0 on success.
 */
static int scan_cache_refresh(void)
{
    s_scan_count = 0;
    if (!wifi_scannable()) {
        return -1;
    }
    const wifi_scan_config_t cfg = {.show_hidden = false};
    if (s_scan_cache == NULL) {
        s_scan_cache = malloc(SCAN_CACHE_MAX * sizeof(*s_scan_cache));
    }
    if (s_scan_raw == NULL) {
        s_scan_raw = malloc(SCAN_CACHE_MAX * sizeof(*s_scan_raw));
    }
    if (s_scan_cache == NULL || s_scan_raw == NULL) {
        ESP_LOGE(TAG, "scan cache OOM");
        return -1;
    }
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed on the radio");
        return -1;
    }
    uint16_t n = SCAN_CACHE_MAX;
    if (esp_wifi_scan_get_ap_records(&n, s_scan_raw) != ESP_OK) {
        return -1;
    }

    /* esp returns records strongest-first; dedup by SSID keeps the first
     * (strongest) hit and skips hidden (empty-SSID) networks. */
    for (uint16_t i = 0; i < n && s_scan_count < SCAN_CACHE_MAX; i++) {
        const wifi_ap_record_t *r = &s_scan_raw[i];
        if (r->ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t k = 0; k < s_scan_count; k++) {
            if (strcmp(s_scan_cache[k].network_id, (const char *)r->ssid) ==
                0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        ct_prov_scan_item_t *item = &s_scan_cache[s_scan_count++];
        strlcpy(item->network_id, (const char *)r->ssid,
                sizeof(item->network_id));
        item->signal = r->rssi;
        item->channel = r->primary;
        item->has_channel = true;
    }
    return 0;
}

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
 * the cached RAM copies of the factory info; the F2 handlers reach NVS and
 * the Wi-Fi radio through the adapters above. Requests are serialized by
 * the host task, so a scan can never land mid-write (doc 9 BUSY is
 * unreachable by construction).
 */
static size_t dispatch_request_scratch(const ct_prov_envelope_t *req, char *resp,
                                       size_t cap, char *scratch, size_t scratch_cap)
{
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
    if (strcmp(req->op, "communication.capabilities.get") == 0) {
        return ct_prov_capabilities_response(
            req->id, wifi_scannable(), current_network_name(), scratch,
            scratch_cap, resp, cap);
    }
    if (strcmp(req->op, "config.get") == 0) {
        return ct_prov_config_get_response(req->id, &s_reader, scratch,
                                           scratch_cap, resp, cap);
    }
    if (strcmp(req->op, "config.set") == 0) {
        return ct_prov_config_set_response(
            req->id, req->data_json, &s_store, scratch,
            scratch_cap, resp, cap);
    }
    if (strcmp(req->op, "communication.scan") == 0) {
        char section[16];
        bool has_cursor = false;
        size_t cursor = 0;
        if (ct_prov_scan_parse(req->data_json, section, sizeof(section),
                               &has_cursor, &cursor) != 0) {
            return error_response(req->id, "communication.scan",
                                  "INVALID_ARGUMENT", "malformed scan request",
                                  resp, cap);
        }
        const bool scannable = wifi_scannable();
        if (strcmp(section, "wifi") != 0 || !scannable) {
            return error_response(
                req->id, "communication.scan", "SECTION_NOT_SUPPORTED",
                scannable ? "section has no scan support"
                          : "wifi radio unavailable on this hardware",
                resp, cap);
        }
        if (!has_cursor && scan_cache_refresh() != 0) {
            return error_response(req->id, "communication.scan",
                                  "INTERNAL_ERROR", "wifi scan failed", resp,
                                  cap);
        }
        return ct_prov_scan_page_response(
            req->id, "wifi", has_cursor, cursor, s_scan_cache, s_scan_count,
            scratch, scratch_cap, resp, cap);
    }
    /* Everything outside the shipped operation set is a typed refusal. */
    return error_response(req->id, req->op, "UNSUPPORTED_OPERATION",
                          "operation not implemented", resp, cap);
}

/* Scratch is per-request heap: between provisioning calls the memory stays
 * available to the RTOS heap (duty cycles never provision). */
static size_t dispatch_request(const ct_prov_envelope_t *req, char *resp,
                               size_t cap)
{
    char *scratch = malloc(CT_PROV_SCRATCH_SIZE);
    if (scratch == NULL) {
        return error_response(req->id, req->op, "INTERNAL_ERROR",
                              "out of memory", resp, cap);
    }
    size_t n = dispatch_request_scratch(req, resp, cap, scratch,
                                        CT_PROV_SCRATCH_SIZE);
    free(scratch);
    return n;
}

/*
 * BLE request callback (doc 13): dispatch under the shared lock because
 * the softAP HTTP binding runs the same handlers on the httpd worker.
 */
static size_t on_request(void *arg, const ct_prov_envelope_t *req, char *resp,
                         size_t cap)
{
    (void)arg;
    if (s_dispatch_mux == NULL) {
        return error_response(req->id, req->op, "INTERNAL_ERROR",
                              "dispatcher unavailable", resp, cap);
    }
    xSemaphoreTake(s_dispatch_mux, portMAX_DELAY);
    const size_t n = dispatch_request(req, resp, cap);
    xSemaphoreGive(s_dispatch_mux);
    return n;
}

size_t ct_prov_app_handle_http(const char *body, size_t len, char *resp,
                               size_t cap)
{
    if (!s_active || body == NULL || len == 0) {
        return 0;
    }

    ct_prov_envelope_t env;
    ct_prov_envelope_error_t err;
    if (ct_prov_envelope_decode((const uint8_t *)body, len, &env, &err) !=
            0 ||
        env.kind != CT_PROV_ENV_REQUEST) {
        /* No request id to echo: refuse with the envelope-level shape. */
        return error_response(0, "protocol.error", "INVALID_MESSAGE",
                              "body is not a CT-Prov request envelope", resp,
                              cap);
    }

    size_t n;
    if (strcmp(env.op, "session.close") == 0) {
        /* Doc 11 lets the client close by ending the transport session.
         * Over HTTP each request is its own connection: acknowledge and
         * carry on; there is no link to terminate. */
        n = ct_prov_envelope_encode_response(env.id, "session.close.result",
                                             true, NULL, NULL, NULL, resp,
                                             cap);
    } else if (s_dispatch_mux == NULL) {
        n = error_response(env.id, env.op, "INTERNAL_ERROR",
                           "dispatcher unavailable", resp, cap);
    } else {
        xSemaphoreTake(s_dispatch_mux, portMAX_DELAY);
        n = dispatch_request(&env, resp, cap);
        xSemaphoreGive(s_dispatch_mux);
    }
    ct_prov_envelope_free(&env);
    return n;
}

esp_err_t ct_prov_app_start(void)
{
    deviceInfo_t dev;
    if (cfg_get_device_info(&dev) != ESP_OK) {
        ESP_LOGE(TAG, "factory device info unreadable");
        return ESP_ERR_INVALID_STATE;
    }

    /* Doc 4.2.2: model codes are a fixed registry; 0x00/0xFF forbidden.
     * Factory model may carry a family suffix (e.g. "NE101-P"); the code
     * maps by family prefix, the full model string stays in device_name. */
    uint8_t model_code;
    if (strncmp(dev.model, "NE101", 5) == 0) {
        model_code = 0x01;
    } else if (strncmp(dev.model, "NE301", 5) == 0) {
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
     * order; MODEL is a single 2-10 uppercase-alnum segment (no interior
     * dash), so a factory model with a family suffix ("NE101-P")
     * advertises by family segment. device.info.get still reports the
     * full factory model. Whether variant names enter the protocol is an
     * open spec question; the registry maps by family prefix. */
    size_t family_len = strcspn(dev.model, "-");
    if (family_len < 2 || family_len > 10) {
        ESP_LOGE(TAG, "model '%s' has no valid family segment", dev.model);
        return ESP_ERR_NOT_SUPPORTED;
    }
    char family[11];
    memcpy(family, dev.model, family_len);
    family[family_len] = '\0';

    ct_prov_ble_identity_t identity = {
        .model_code = model_code,
        .mac_suffix = {mac[3], mac[4], mac[5]},
    };
    char name[24];
    const int n = snprintf(name, sizeof(name), "%s-%02X%02X%02X",
                           family, mac[3], mac[4], mac[5]);
    if (n < 0 || (size_t)n >= sizeof(identity.device_name)) {
        ESP_LOGE(TAG, "family '%s' yields an over-long device name", family);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(identity.device_name, name, (size_t)n + 1);

    s_dev = dev;
    s_identity = identity;
    if (s_dispatch_mux == NULL) {
        s_dispatch_mux = xSemaphoreCreateMutex();
    }
    const esp_err_t rc = ct_prov_ble_start(&s_identity, on_request, NULL);
    s_active = rc == ESP_OK;
    return rc;
}

void ct_prov_app_stop(void)
{
    s_active = false;
    ct_prov_ble_stop();
    free(s_scan_cache);
    s_scan_cache = NULL;
    free(s_scan_raw);
    s_scan_raw = NULL;
    s_scan_count = 0;
}
