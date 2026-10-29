/*
 * Host tests for the CT-Prov discovery payload builders.
 * Expected bytes come straight from the examples in protocol doc sections
 * 4.2.2, 4.2.3 and 4.3, so this file doubles as a byte-layout regression
 * lock for the advertising layout.
 */
#include <stdio.h>
#include <string.h>

#include "ct_prov_adv.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ctx, fmt, ...)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                          \
            g_failures++;                                                      \
            printf("FAIL [%s] %s:%d: " fmt "\n", ctx, __func__, __LINE__,       \
                   ##__VA_ARGS__);                                              \
        }                                                                      \
    } while (0)

/* Full Advertising Data example from doc 4.2.2 (NE101, A1:B2:C3). */
static const uint8_t k_adv_example[CT_PROV_ADV_LEN] = {
    0x02, 0x01, 0x06,
    0x17, 0x21,
    0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
    0x5b, 0x4b, 0x5b, 0x9d, 0x01, 0x00, 0x1e, 0x7b,
    0x01, 0x01, 0xa1, 0xb2, 0xc3, 0x02,
};

/* Complete Local Name example from doc 4.2.3. */
static const uint8_t k_scan_rsp_example[] = {
    0x0d, 0x09, 'N', 'E', '1', '0', '1', '-', 'A', '1', 'B', '2', 'C', '3',
};

static void test_adv_example(void)
{
    ct_prov_discovery_t disc = {
        .model_code = 0x01,
        .mac_suffix = {0xa1, 0xb2, 0xc3},
        .flags = CT_PROV_FLAG_PROVISIONABLE,
    };
    uint8_t out[CT_PROV_ADV_LEN];
    const int n = ct_prov_adv_build(&disc, out, sizeof(out));
    CHECK(n == CT_PROV_ADV_LEN, "adv-example", "len %d want 27", n);
    CHECK(n == CT_PROV_ADV_LEN &&
              memcmp(out, k_adv_example, sizeof(out)) == 0,
          "adv-example", "bytes differ from doc example");

    /* The UUID slice inside the built data must match the doc byte order. */
    const uint8_t *uuid = ct_prov_uuid_le();
    static const uint8_t k_uuid_doc[CT_PROV_UUID_LE_LEN] = {
        0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
        0x5b, 0x4b, 0x5b, 0x9d, 0x01, 0x00, 0x1e, 0x7b,
    };
    CHECK(memcmp(uuid, k_uuid_doc, CT_PROV_UUID_LE_LEN) == 0, "adv-example",
          "uuid little-endian order differs from doc");
    CHECK(memcmp(out + 5, uuid, CT_PROV_UUID_LE_LEN) == 0, "adv-example",
          "uuid must sit at offset 5");
}

static void test_adv_rejects(void)
{
    uint8_t out[CT_PROV_ADV_LEN];
    ct_prov_discovery_t disc = {
        .model_code = 0x01,
        .mac_suffix = {0xa1, 0xb2, 0xc3},
        .flags = CT_PROV_FLAG_PROVISIONABLE,
    };

    CHECK(ct_prov_adv_build(NULL, out, sizeof(out)) == -1, "adv-reject",
          "null discovery must be refused");
    CHECK(ct_prov_adv_build(&disc, NULL, sizeof(out)) == -1, "adv-reject",
          "null out must be refused");
    CHECK(ct_prov_adv_build(&disc, out, CT_PROV_ADV_LEN - 1) == -1,
          "adv-reject", "cap 26 must be refused");

    disc.flags = CT_PROV_FLAG_BUSY | CT_PROV_FLAG_PROVISIONABLE;
    CHECK(ct_prov_adv_build(&disc, out, sizeof(out)) == -1, "adv-reject",
          "BUSY with PROVISIONABLE must be refused");

    disc.flags = 0x04; /* reserved bit */
    CHECK(ct_prov_adv_build(&disc, out, sizeof(out)) == -1, "adv-reject",
          "reserved flag bits must be refused");

    disc.flags = CT_PROV_FLAG_BUSY; /* BUSY alone is legal */
    CHECK(ct_prov_adv_build(&disc, out, sizeof(out)) == CT_PROV_ADV_LEN,
          "adv-reject", "BUSY without PROVISIONABLE must build");

    disc.model_code = 0x00;
    CHECK(ct_prov_adv_build(&disc, out, sizeof(out)) == -1, "adv-reject",
          "model_code 0x00 must be refused");
    disc.model_code = 0xFF;
    CHECK(ct_prov_adv_build(&disc, out, sizeof(out)) == -1, "adv-reject",
          "model_code 0xFF must be refused");
}

static void test_scan_rsp(void)
{
    uint8_t out[CT_PROV_SCAN_RSP_MAX];
    const int n =
        ct_prov_scan_rsp_build("NE101-A1B2C3", out, sizeof(out));
    CHECK(n == (int)sizeof(k_scan_rsp_example), "scan-rsp", "len %d", n);
    CHECK(n == (int)sizeof(k_scan_rsp_example) &&
              memcmp(out, k_scan_rsp_example, n) == 0,
          "scan-rsp", "bytes differ from doc example");

    /* Shortest legal name: MODEL(2) + '-' + 6 hex. */
    CHECK(ct_prov_scan_rsp_build("N1-A1B2C3", out, sizeof(out)) == 11,
          "scan-rsp", "9-byte name must build");
    /* Longest legal name: MODEL(10) + '-' + 6 hex = 17 bytes. */
    CHECK(ct_prov_scan_rsp_build("NE10123456-A1B2C3", out, sizeof(out)) == 19,
          "scan-rsp", "17-byte name must build");
    CHECK(ct_prov_scan_rsp_build("NE101-A1B2C3", out, 13) == -1, "scan-rsp",
          "cap shorter than output must be refused");

    CHECK(ct_prov_scan_rsp_build("N-A1B2C3", out, sizeof(out)) == -1,
          "scan-rsp", "8-byte name must be refused");
    CHECK(ct_prov_scan_rsp_build("NE101234567-A1B2C3", out, sizeof(out)) == -1,
          "scan-rsp", "18-byte name must be refused");
    CHECK(ct_prov_scan_rsp_build("ne101-a1b2c3", out, sizeof(out)) == -1,
          "scan-rsp", "lowercase must be refused");
    CHECK(ct_prov_scan_rsp_build("NE_1-A1B2C3", out, sizeof(out)) == -1,
          "scan-rsp", "underscore must be refused");
    CHECK(ct_prov_scan_rsp_build("NE101-a1b2c3", out, sizeof(out)) == -1,
          "scan-rsp", "lowercase suffix must be refused");
}

static void test_meta(void)
{
    uint8_t out[CT_PROV_META_MAX];
    const int n = ct_prov_meta_build("NE101-A1B2C3", CT_PROV_FLAG_PROVISIONABLE,
                                     out, sizeof(out));
    CHECK(n == 15, "meta", "len %d want 15", n);
    CHECK(n == 15 && out[0] == 0x01, "meta", "protocol_version must be 1");
    CHECK(n == 15 && out[1] == CT_PROV_FLAG_PROVISIONABLE, "meta", "flags");
    CHECK(n == 15 && out[2] == 12, "meta", "name_length must be 12");
    CHECK(n == 15 && memcmp(out + 3, "NE101-A1B2C3", 12) == 0, "meta",
          "name bytes");

    /* Longest name lands exactly on the 20-byte META cap. */
    CHECK(ct_prov_meta_build("NE10123456-A1B2C3", 0, out, sizeof(out)) == 20,
          "meta", "17-byte name must hit the cap exactly");
    CHECK(ct_prov_meta_build("NE101-A1B2C3", 0, out, 14) == -1, "meta",
          "cap shorter than output must be refused");

    CHECK(ct_prov_meta_build("ne101-a1b2c3", 0, out, sizeof(out)) == -1,
          "meta", "invalid name must be refused");
    CHECK(ct_prov_meta_build("NE101-A1B2C3", 0x03, out, sizeof(out)) == -1,
          "meta", "BUSY with PROVISIONABLE must be refused");
    CHECK(ct_prov_meta_build("NE101-A1B2C3", 0x80, out, sizeof(out)) == -1,
          "meta", "reserved flag bits must be refused");
}

static void test_name_valid(void)
{
    CHECK(ct_prov_name_valid("NE101-A1B2C3"), "name", "doc example");
    CHECK(ct_prov_name_valid("N1-A1B2C3"), "name", "minimum length");
    CHECK(ct_prov_name_valid("NE-A1B2C3"), "name", "model at 2-char minimum");
    CHECK(ct_prov_name_valid("NE10123456-A1B2C3"), "name", "maximum length");
    CHECK(!ct_prov_name_valid("NE101234567-A1B2C3"), "name", "too long");
    CHECK(!ct_prov_name_valid("N-A1B2C3"), "name", "model too short");
    CHECK(!ct_prov_name_valid("NE101-A1B2C"), "name", "suffix too short");
    CHECK(!ct_prov_name_valid("NE101-A1B2C33"), "name", "suffix too long");
    CHECK(!ct_prov_name_valid("NE101-G1H2J3"), "name", "suffix not hex");
    CHECK(!ct_prov_name_valid("NE-101-A1B2C3"), "name", "second dash");
    CHECK(!ct_prov_name_valid("NE101A1B2C3"), "name", "no dash");
    CHECK(!ct_prov_name_valid("NE101_A1B2C3"), "name", "underscore");
    CHECK(!ct_prov_name_valid("NE101-a1b2c3"), "name", "lowercase");
    CHECK(!ct_prov_name_valid(""), "name", "empty");
    CHECK(!ct_prov_name_valid(NULL), "name", "null");
}

int main(void)
{
    test_adv_example();
    test_adv_rejects();
    test_scan_rsp();
    test_meta();
    test_name_valid();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
