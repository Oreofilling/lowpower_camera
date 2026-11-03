/*
 * Host tests for the F2 config.set handler: the validation matrix, the
 * all-or-nothing guarantee, and the default backfill for omitted optional
 * fields. A counting fixture store records every write so the tests can
 * prove what reached storage.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "ct_prov_config.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ctx, fmt, ...)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                          \
            g_failures++;                                                      \
            printf("FAIL [%s] %s:%d: " fmt "\n", ctx, __func__, __LINE__,       \
                   ##__VA_ARGS__);                                             \
        }                                                                      \
    } while (0)

#define RESP_CAP 4096

/* ---- counting fixture store -------------------------------------------- */

#define W_MAX 128
typedef struct {
    char key[32];
    char val[64];
    int calls;
} wrec_t;

static wrec_t g_w[W_MAX];
static int g_wn;
static int g_wfail; /* nonzero: every write fails */

static void w_reset(void)
{
    g_wn = 0;
    g_wfail = 0;
    memset(g_w, 0, sizeof(g_w));
}

static wrec_t *w_find(const char *key)
{
    for (int i = 0; i < g_wn; i++) {
        if (strcmp(g_w[i].key, key) == 0) {
            return &g_w[i];
        }
    }
    return NULL;
}

static void w_record(const char *key, const char *val)
{
    wrec_t *r = w_find(key);
    if (r == NULL && g_wn < W_MAX) {
        r = &g_w[g_wn++];
        snprintf(r->key, sizeof(r->key), "%s", key);
    }
    if (r != NULL) {
        snprintf(r->val, sizeof(r->val), "%s", val);
        r->calls++;
    }
}

static int w_str(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    w_record(key, value);
    return g_wfail ? -1 : 0;
}

static int w_u8(void *ctx, const char *key, uint8_t value)
{
    (void)ctx;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)value);
    w_record(key, buf);
    return g_wfail ? -1 : 0;
}

static int w_u32(void *ctx, const char *key, uint32_t value)
{
    (void)ctx;
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
    w_record(key, buf);
    return g_wfail ? -1 : 0;
}

static const ct_prov_store_t k_store = {
    .ctx = NULL, .write_str = w_str, .write_u8 = w_u8, .write_u32 = w_u32};

static const char *w_val(const char *key)
{
    const wrec_t *r = w_find(key);
    return r != NULL ? r->val : NULL;
}

static int w_calls(const char *key)
{
    const wrec_t *r = w_find(key);
    return r != NULL ? r->calls : 0;
}

/* Run one config.set and parse the response back. */
static cJSON *do_set(const char *data, int64_t id)
{
    static char scratch[2048];
    static char resp[RESP_CAP];
    w_reset();
    const size_t n = ct_prov_config_set_response(id, data, &k_store, scratch,
                                                  sizeof(scratch), resp,
                                                  sizeof(resp));
    CHECK(n > 0, "set", "response length %zu", n);
    return cJSON_Parse(resp);
}

/* Assert the envelope of an ok response and return its data object. */
static const cJSON *ok_data(const cJSON *root, const char *ctx)
{
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(root, "ok")), ctx, "ok=true");
    CHECK(strcmp(cJSON_GetObjectItem(root, "op")->valuestring,
                 "config.set.result") == 0,
          ctx, "op is config.set.result");
    return cJSON_GetObjectItem(root, "data");
}

static const cJSON *err(const cJSON *root, const char *ctx, const char *code)
{
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "ok")), ctx, "ok=false");
    const cJSON *e = cJSON_GetObjectItem(root, "error");
    CHECK(e != NULL, ctx, "error object present");
    if (e != NULL) {
        CHECK(strcmp(cJSON_GetObjectItem(e, "code")->valuestring, code) == 0,
              ctx, "error.code %s", code);
    }
    return e;
}

/* ---- happy paths ------------------------------------------------------- */

static void test_set_wifi_minimal(void)
{
    cJSON *root = do_set("{\"wifi\":{\"ssid\":\"Home\"}}", 21);
    CHECK(root != NULL, "set-wifi", "parse");
    if (root == NULL) {
        return;
    }
    const cJSON *data = ok_data(root, "set-wifi");
    const cJSON *stored = cJSON_GetObjectItem(data, "stored");
    CHECK(cJSON_GetArraySize(stored) == 1, "set-wifi", "stored has wifi");
    CHECK(strcmp(cJSON_GetArrayItem(stored, 0)->valuestring, "wifi") == 0,
          "set-wifi", "stored[0] wifi");
    CHECK(strcmp(cJSON_GetObjectItem(data, "apply")->valuestring,
                 "reconnect") == 0,
          "set-wifi", "apply reconnect");
    /* Omitted optional password is written back at its schema default. */
    CHECK(w_val("wifi:ssid") != NULL && strcmp(w_val("wifi:ssid"), "Home") == 0,
          "set-wifi", "ssid written");
    CHECK(w_val("wifi:password") != NULL, "set-wifi",
          "password default written");
    cJSON_Delete(root);
}

static void test_set_mqtt_full(void)
{
    cJSON *root = do_set(
        "{\"mqtt\":{\"enable\":true,\"host\":\"h.example.com\",\"port\":8883,"
        "\"client_id\":\"cam-1\",\"username\":\"u\",\"password\":\"p\","
        "\"tls\":true,\"topic\":\"cam/up\",\"qos\":2}}",
        22);
    CHECK(root != NULL, "set-mqtt", "parse");
    if (root == NULL) {
        return;
    }
    ok_data(root, "set-mqtt");
    CHECK(w_calls("mqtt:enable") == 1 && strcmp(w_val("mqtt:enable"), "1") == 0,
          "set-mqtt", "enable stored as u8 1");
    CHECK(w_val("mqtt:host") != NULL && strcmp(w_val("mqtt:host"),
                                               "h.example.com") == 0,
          "set-mqtt", "host written");
    CHECK(w_val("mqtt:port") != NULL && strcmp(w_val("mqtt:port"), "8883") == 0,
          "set-mqtt", "port u32");
    CHECK(w_val("mqtt:clientId") != NULL &&
              strcmp(w_val("mqtt:clientId"), "cam-1") == 0,
          "set-mqtt", "client_id");
    CHECK(w_val("mqtt:user") != NULL && strcmp(w_val("mqtt:user"), "u") == 0,
          "set-mqtt", "username");
    CHECK(w_val("mqtt:password") != NULL && strcmp(w_val("mqtt:password"),
                                                   "p") == 0,
          "set-mqtt", "password in clear to store only");
    CHECK(strcmp(w_val("mqtt:tlsEnable"), "1") == 0, "set-mqtt", "tls u8");
    CHECK(strcmp(w_val("mqtt:topic"), "cam/up") == 0, "set-mqtt", "topic");
    CHECK(strcmp(w_val("mqtt:qos"), "2") == 0, "set-mqtt", "qos u8");
    cJSON_Delete(root);
}

static void test_set_cellular_enum_and_defaults(void)
{
    cJSON *root = do_set(
        "{\"cellular\":{\"apn\":\"cmnet\",\"auth\":\"chap\",\"pin\":\"1234\"}}",
        23);
    CHECK(root != NULL, "set-cat1", "parse");
    if (root == NULL) {
        return;
    }
    ok_data(root, "set-cat1");
    CHECK(strcmp(w_val("cat1:apn"), "cmnet") == 0, "set-cat1", "apn");
    CHECK(strcmp(w_val("cat1:authType"), "2") == 0, "set-cat1",
          "auth chap = index 2");
    CHECK(strcmp(w_val("cat1:pin"), "1234") == 0, "set-cat1", "pin");
    CHECK(w_val("cat1:user") != NULL && w_val("cat1:password") != NULL,
          "set-cat1", "omitted optionals defaulted");
    cJSON_Delete(root);
}

static void test_set_multi_section_order(void)
{
    cJSON *root = do_set(
        "{\"webhook\":{\"enable\":true,\"url\":\"https://a.io/h\"},"
        "\"wifi\":{\"ssid\":\"x\"}}",
        24);
    CHECK(root != NULL, "set-multi", "parse");
    if (root == NULL) {
        return;
    }
    const cJSON *data = ok_data(root, "set-multi");
    const cJSON *stored = cJSON_GetObjectItem(data, "stored");
    CHECK(cJSON_GetArraySize(stored) == 2, "set-multi", "two sections");
    CHECK(strcmp(cJSON_GetArrayItem(stored, 0)->valuestring, "webhook") == 0,
          "set-multi", "request order kept (webhook first)");
    CHECK(strcmp(cJSON_GetArrayItem(stored, 1)->valuestring, "wifi") == 0,
          "set-multi", "wifi second");
    CHECK(strcmp(w_val("push:mode"), "1") == 0, "set-multi",
          "webhook enable via push:mode");
    cJSON_Delete(root);
}

static void test_set_empty_object(void)
{
    cJSON *root = do_set("{}", 25);
    CHECK(root != NULL, "set-empty", "parse");
    if (root != NULL) {
        const cJSON *data = ok_data(root, "set-empty");
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(data, "stored")) == 0,
              "set-empty", "stored empty");
        CHECK(g_wn == 0, "set-empty", "nothing written");
        cJSON_Delete(root);
    }
}

/* ---- all-or-nothing ---------------------------------------------------- */

static void test_set_all_or_nothing(void)
{
    /* wifi validates, mqtt does not (host/port missing): nothing is written
     * even though the wifi section alone would pass. */
    cJSON *root = do_set(
        "{\"wifi\":{\"ssid\":\"good\"},\"mqtt\":{\"enable\":true}}", 26);
    CHECK(root != NULL, "atomic", "parse");
    if (root == NULL) {
        return;
    }
    err(root, "atomic", "INVALID_ARGUMENT");
    CHECK(g_wn == 0, "atomic", "zero writes after rejected request");
    cJSON_Delete(root);
}

/* ---- validation matrix -------------------------------------------------- */

static void expect_reject(const char *data, const char *code,
                          const char *needle, int line)
{
    char ctx[32];
    snprintf(ctx, sizeof(ctx), "reject:%d", line);
    cJSON *root = do_set(data, 30 + line % 100);
    CHECK(root != NULL, ctx, "parse");
    if (root == NULL) {
        return;
    }
    const cJSON *e = err(root, ctx, code);
    if (e != NULL && needle != NULL) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        CHECK(msg != NULL && strstr(msg, needle) != NULL, ctx,
              "message names %s (got %s)", needle, msg != NULL ? msg : "?");
    }
    CHECK(g_wn == 0, ctx, "rejected request wrote nothing");
    cJSON_Delete(root);
}

static void test_set_validation_matrix(void)
{
    expect_reject("{\"ethernet\":{\"ssid\":\"x\"}}", "SECTION_NOT_SUPPORTED",
                  "ethernet", __LINE__);
    expect_reject("{\"wifi\":{\"ssid\":\"x\",\"bssid\":\"aa\"}}",
                  "FIELD_NOT_SUPPORTED", "bssid", __LINE__);
    expect_reject("{\"wifi\":{\"password\":\"12345678\"}}", "INVALID_ARGUMENT",
                  "ssid", __LINE__); /* required missing */
    expect_reject("{\"wifi\":{\"ssid\":123}}", "INVALID_ARGUMENT", "ssid",
                  __LINE__); /* wrong type */
    expect_reject("{\"wifi\":{\"ssid\":\"\"}}", "INVALID_ARGUMENT", "ssid",
                  __LINE__);
    expect_reject("{\"wifi\":{\"ssid\":\"0123456789012345678901234567890X\"}}",
                  "INVALID_ARGUMENT", "ssid", __LINE__); /* 32 bytes */
    expect_reject("{\"wifi\":{\"ssid\":\"ok\",\"password\":\"short\"}}",
                  "INVALID_ARGUMENT", "password", __LINE__); /* 5 bytes */
    expect_reject("{\"mqtt\":{\"enable\":true,\"host\":\"h\",\"port\":0}}",
                  "INVALID_ARGUMENT", "port", __LINE__);
    expect_reject("{\"mqtt\":{\"enable\":true,\"host\":\"h\",\"port\":65536}}",
                  "INVALID_ARGUMENT", "port", __LINE__);
    expect_reject("{\"mqtt\":{\"enable\":true,\"host\":\"h\",\"port\":1883.5}}",
                  "INVALID_ARGUMENT", "port", __LINE__); /* non-integral */
    expect_reject("{\"mqtt\":{\"host\":\"h\",\"port\":1883}}", "INVALID_ARGUMENT",
                  "enable", __LINE__); /* required missing */
    expect_reject(
        "{\"mqtt\":{\"enable\":\"yes\",\"host\":\"h\",\"port\":1883}}",
        "INVALID_ARGUMENT", "enable", __LINE__); /* bool type */
    expect_reject("{\"mqtt\":{\"enable\":true,\"host\":\"h\",\"port\":1883,"
                  "\"qos\":3}}",
                  "INVALID_ARGUMENT", "qos", __LINE__);
    expect_reject(
        "{\"mqtt\":{\"enable\":true,\"host\":\"h\",\"port\":1883,"
        "\"topic\":\"a/+/b\"}}",
        "INVALID_ARGUMENT", "topic", __LINE__); /* wildcard */
    expect_reject("{\"webhook\":{\"enable\":true,\"url\":\"ftp://a.io\"}}",
                  "INVALID_ARGUMENT", "url", __LINE__);
    expect_reject("{\"webhook\":{\"enable\":true,\"url\":\"short\"}}",
                  "INVALID_ARGUMENT", "url", __LINE__);
    expect_reject("{\"cellular\":{\"apn\":\"a\",\"pin\":\"12a4\"}}",
                  "INVALID_ARGUMENT", "pin", __LINE__);
    expect_reject("{\"cellular\":{\"apn\":\"a\",\"auth\":\"wpa\"}}",
                  "INVALID_ARGUMENT", "auth", __LINE__);
    expect_reject("{\"cellular\":{\"apn\":\"a\",\"extra\":1}}",
                  "FIELD_NOT_SUPPORTED", "extra", __LINE__);
    expect_reject("{\"wifi\":\"flat\"}", "INVALID_ARGUMENT", "wifi",
                  __LINE__); /* section not an object */
    expect_reject("{oops", "INVALID_ARGUMENT", NULL, __LINE__); /* bad json */
    expect_reject("\"flat\"", "INVALID_ARGUMENT", NULL, __LINE__);
}

static void test_set_storage_failed(void)
{
    static char scratch[2048];
    static char resp[RESP_CAP];
    w_reset();
    g_wfail = 1;
    const size_t n = ct_prov_config_set_response(
        27, "{\"wifi\":{\"ssid\":\"Home\"}}", &k_store, scratch,
        sizeof(scratch), resp, sizeof(resp));
    CHECK(n > 0, "storage", "response length %zu", n);
    cJSON *root = cJSON_Parse(resp);
    CHECK(root != NULL, "storage", "parse");
    if (root != NULL) {
        err(root, "storage", "STORAGE_FAILED");
        cJSON_Delete(root);
    }
}

int main(void)
{
    test_set_wifi_minimal();
    test_set_mqtt_full();
    test_set_cellular_enum_and_defaults();
    test_set_multi_section_order();
    test_set_empty_object();
    test_set_all_or_nothing();
    test_set_validation_matrix();
    test_set_storage_failed();
    printf("set: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
