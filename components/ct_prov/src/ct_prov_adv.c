/*
 * CT-Prov/1 discovery payload builders (protocol doc section 4).
 * Byte layouts and validation rules mirror doc sections 4.2.1-4.3; the
 * host tests lock them against the examples printed in the doc.
 */
#include "ct_prov_adv.h"

#include <string.h>

/* 7B1E0001-9D5B-4B5B-8F6D-2A4C0B7E0100 in BLE little-endian order. */
static const uint8_t k_uuid_le[CT_PROV_UUID_LE_LEN] = {
    0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
    0x5b, 0x4b, 0x5b, 0x9d, 0x01, 0x00, 0x1e, 0x7b,
};

const uint8_t *ct_prov_uuid_le(void)
{
    return k_uuid_le;
}

static bool flags_valid(uint8_t flags)
{
    if ((flags & ~(CT_PROV_FLAG_BUSY | CT_PROV_FLAG_PROVISIONABLE)) != 0) {
        return false; /* reserved bits must be zero */
    }
    if ((flags & CT_PROV_FLAG_BUSY) &&
        (flags & CT_PROV_FLAG_PROVISIONABLE)) {
        return false; /* BUSY=1 forces PROVISIONABLE=0 */
    }
    return true;
}

/* Doc 4.2.1: MODEL(2..10 uppercase alnum) + '-' + MAC_SUFFIX(6 uppercase
 * hex), 9..17 bytes total. The structure rule is stricter than the charset
 * rule alone: exactly one dash and a hex-only suffix. */
bool ct_prov_name_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t len = strlen(name);
    if (len < 9 || len > 17) {
        return false;
    }
    const char *dash = memchr(name, '-', len);
    if (dash == NULL) {
        return false;
    }
    const size_t model_len = (size_t)(dash - name);
    if (model_len < 2 || model_len > 10) {
        return false;
    }
    if (len - model_len - 1 != 6) {
        return false; /* suffix must be exactly 6 bytes */
    }
    if (memchr(dash + 1, '-', len - model_len - 1) != NULL) {
        return false; /* exactly one dash */
    }
    for (size_t i = 0; i < model_len; i++) {
        const char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    for (size_t i = model_len + 1; i < len; i++) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false; /* uppercase hex only */
        }
    }
    return true;
}

int ct_prov_adv_build(const ct_prov_discovery_t *disc, uint8_t *out,
                      size_t cap)
{
    if (disc == NULL || out == NULL || cap < CT_PROV_ADV_LEN) {
        return -1;
    }
    if (disc->model_code == 0x00 || disc->model_code == 0xFF) {
        return -1;
    }
    if (!flags_valid(disc->flags)) {
        return -1;
    }

    /* Flags AD structure. */
    out[0] = 0x02; /* Length */
    out[1] = 0x01; /* AD type: Flags */
    out[2] = 0x06; /* General Discoverable | BR/EDR Not Supported */

    /* Service Data - 128-bit UUID AD structure. */
    out[3] = 0x17; /* Length: 23 bytes after this field */
    out[4] = 0x21; /* AD type: Service Data - 128-bit UUID */
    memcpy(out + 5, k_uuid_le, CT_PROV_UUID_LE_LEN);
    out[21] = 0x01; /* format_version: CT-Prov/1 */
    out[22] = disc->model_code;
    memcpy(out + 23, disc->mac_suffix, 3);
    out[26] = disc->flags;
    return CT_PROV_ADV_LEN;
}

int ct_prov_scan_rsp_build(const char *device_name, uint8_t *out, size_t cap)
{
    if (!ct_prov_name_valid(device_name) || out == NULL) {
        return -1;
    }
    const size_t name_len = strlen(device_name);
    const size_t total = 2 + name_len;
    if (cap < total) {
        return -1;
    }
    out[0] = (uint8_t)(1 + name_len); /* AD type + name, without Length */
    out[1] = 0x09;                    /* AD type: Complete Local Name */
    memcpy(out + 2, device_name, name_len);
    return (int)total;
}

int ct_prov_meta_build(const char *device_name, uint8_t flags, uint8_t *out,
                       size_t cap)
{
    if (!ct_prov_name_valid(device_name) || out == NULL || !flags_valid(flags)) {
        return -1;
    }
    const size_t name_len = strlen(device_name);
    if (cap < 3 + name_len) {
        return -1;
    }
    out[0] = 0x01; /* protocol_version */
    out[1] = flags;
    out[2] = (uint8_t)name_len;
    memcpy(out + 3, device_name, name_len);
    return (int)(3 + name_len);
}
