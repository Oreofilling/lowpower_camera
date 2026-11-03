#ifndef __CT_PROV_CONFIG_H__
#define __CT_PROV_CONFIG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CT-Prov/1 F2 configuration plane (protocol doc sections 8-10 plus
 * CT_CONFIG_SCHEMA.md): NE101 capability declaration, all-or-nothing
 * config.set validation over an injected persistence sink, masked
 * config.get read-back, and cursor-paged scan responses.
 *
 * The layer is host-testable: no ESP-IDF calls here. Storage and radio
 * access stay behind the callback tables below. NVS key names mirror the
 * NE101 mapping table in CT_CONFIG_SCHEMA.md 10.1 and the KEY_* macros in
 * main/config.h. Handlers run serialized on the caller's thread (the
 * NimBLE host task on target), matching the F1 request contract.
 */

/* Persistence sink for validated fields (config.set). Return 0 on success,
 * non-zero to surface STORAGE_FAILED. */
typedef struct ct_prov_store {
    void *ctx;
    int (*write_str)(void *ctx, const char *key, const char *value);
    int (*write_u8)(void *ctx, const char *key, uint8_t value);
    int (*write_u32)(void *ctx, const char *key, uint32_t value);
} ct_prov_store_t;

/* Read side for config.get. read_str always writes a NUL-terminated value
 * ("" when unset); the numeric readers return false when the key is unset. */
typedef struct ct_prov_reader {
    void *ctx;
    void (*read_str)(void *ctx, const char *key, char *out, size_t cap);
    bool (*read_u8)(void *ctx, const char *key, uint8_t *out);
    bool (*read_u32)(void *ctx, const char *key, uint32_t *out);
} ct_prov_reader_t;

/* One cached radio scan entry (wifi section). */
typedef struct ct_prov_scan_item {
    char network_id[33]; /* SSID, NUL-terminated */
    int16_t signal;      /* RSSI in dBm */
    uint8_t channel;     /* primary channel */
    bool has_channel;    /* radio backends without channel info omit it */
} ct_prov_scan_item_t;

/*
 * Every handler encodes one complete response envelope into resp and
 * returns its length, or 0 when even the error response does not fit
 * (the GATT layer then answers INTERNAL_ERROR). scratch is the
 * caller-provided assembly buffer for the data object.
 */
size_t ct_prov_capabilities_response(int64_t id, bool wifi_scannable,
                                     const char *current_network,
                                     char *scratch, size_t scratch_cap,
                                     char *resp, size_t resp_cap);

size_t ct_prov_config_set_response(int64_t id, const char *data_json,
                                   const ct_prov_store_t *store,
                                   char *scratch, size_t scratch_cap,
                                   char *resp, size_t resp_cap);

size_t ct_prov_config_get_response(int64_t id, const ct_prov_reader_t *reader,
                                   char *scratch, size_t scratch_cap,
                                   char *resp, size_t resp_cap);

/*
 * communication.scan request body: {"section":"wifi","cursor":"2"} with
 * cursor optional. Returns 0 on success; -1 means the body is malformed
 * (caller answers INVALID_ARGUMENT). An empty cursor string counts as
 * absent (first page).
 */
int ct_prov_scan_parse(const char *data_json, char *section,
                       size_t section_cap, bool *has_cursor, size_t *cursor);

/*
 * Serves one page of at most CT_PROV_SCAN_PAGE_MAX items from an
 * already-gathered array. cursor is an opaque index; values past count are
 * rejected as stale (doc 9). The item array must stay valid until the
 * response has been encoded.
 */
#define CT_PROV_SCAN_PAGE_MAX 32

size_t ct_prov_scan_page_response(int64_t id, const char *section,
                                  bool has_cursor, size_t cursor,
                                  const ct_prov_scan_item_t *items,
                                  size_t count, char *scratch,
                                  size_t scratch_cap, char *resp,
                                  size_t resp_cap);

#ifdef __cplusplus
}
#endif

#endif /* __CT_PROV_CONFIG_H__ */
