/*
 * CT-Prov/1 discovery payload builders (protocol doc section 4).
 * Pure byte packing with full validation, so the GATT layer can hand the
 * output straight to the controller and host tests can check it against the
 * byte examples in the doc.
 */
#ifndef CT_PROV_ADV_H
#define CT_PROV_ADV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CT-Prov Service UUID 7B1E0001-9D5B-4B5B-8F6D-2A4C0B7E0100 in BLE
 * little-endian advertising order (doc 4.2.2). */
#define CT_PROV_UUID_LE_LEN 16

/* Fixed Advertising Data length: Flags (3) + Service Data structure (24). */
#define CT_PROV_ADV_LEN 27

/* Scan Response: AD header (2) + device_name (9..17). */
#define CT_PROV_SCAN_RSP_MAX 19

/* META: protocol_version + flags + name_length + name (doc 4.3). */
#define CT_PROV_META_MAX 20

/* Discovery flags bitmap (doc 4.2.2); bits 2..7 reserved, must be zero. */
#define CT_PROV_FLAG_BUSY 0x01
#define CT_PROV_FLAG_PROVISIONABLE 0x02

/* Discovery payload carried inside Service Data; format_version is fixed
 * to 0x01 by the builder and not part of this struct. */
typedef struct {
    uint8_t model_code;    /* 0x01 NE101, 0x02 NE301; 0x00/0xFF forbidden */
    uint8_t mac_suffix[3]; /* canonical MAC last 3 bytes, display order */
    uint8_t flags;         /* CT_PROV_FLAG_* */
} ct_prov_discovery_t;

/*
 * Build the 27-byte Advertising Data (doc 4.2.2):
 *   02 01 06 | 17 21 | uuid_le(16) | version(1) model(1) mac(3) flags(1)
 * Returns CT_PROV_ADV_LEN, or -1 on invalid arguments, reserved flag bits,
 * BUSY with PROVISIONABLE, or a forbidden model_code.
 */
int ct_prov_adv_build(const ct_prov_discovery_t *disc, uint8_t *out,
                      size_t cap);

/*
 * Build the Scan Response carrying the Complete Local Name (doc 4.2.3):
 *   len 09 <device_name>
 * Returns the total length, or -1 when the name is not 9..17 bytes of
 * A-Z, 0-9 and '-'.
 */
int ct_prov_scan_rsp_build(const char *device_name, uint8_t *out, size_t cap);

/*
 * Build the META characteristic value (doc 4.3):
 *   protocol_version(0x01) flags name_length name
 * Returns the total length (<= CT_PROV_META_MAX), or -1 on an invalid name
 * or flag combination.
 */
int ct_prov_meta_build(const char *device_name, uint8_t flags, uint8_t *out,
                       size_t cap);

/* device_name rule (doc 4.2.1): MODEL(2..10)-MAC_SUFFIX(6), 9..17 bytes of
 * A-Z, 0-9, '-'. */
bool ct_prov_name_valid(const char *name);

/* The 16 little-endian service UUID bytes used inside Service Data. */
const uint8_t *ct_prov_uuid_le(void);

#endif /* CT_PROV_ADV_H */
