/*
 * Host tests for the F2 capabilities and config.get handlers. Responses are
 * parsed back with cJSON so the assertions lock the wire shape, not the
 * serialization details.
 */
#include <stdio.h>
#include <stdlib.h>
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

/* ---- capabilities ------------------------------------------------------ */

static void test_capabilities_shape(void)
{
    char scratch[2048];
    char resp[RESP_CAP];
    const size_t n = ct_prov_capabilities_response(
        7, true, NULL, scratch, sizeof(scratch), resp, sizeof(resp));
    CHECK(n > 0, "caps", "response length %zu", n);

    cJSON *root = cJSON_Parse(resp);
    CHECK(root != NULL, "caps", "response is valid JSON");
    if (root == NULL) {
        return;
    }
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(root, "ok")), "caps", "ok=true");
    CHECK(strcmp(cJSON_GetObjectItem(root, "op")->valuestring,
                 "communication.capabilities.get.result") == 0,
          "caps", "op echoes request + .result");
    CHECK(cJSON_GetObjectItem(root, "id")->valueint == 7, "caps", "id echo");

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *sections = cJSON_GetObjectItem(data, "sections");
    CHECK(cJSON_GetArraySize(sections) == 4, "caps", "NE101 declares 4 sections");
    const char *k_names[] = {"wifi", "cellular", "mqtt", "webhook"};
    const char *k_kinds[] = {"network", "network", "service", "service"};
    for (int i = 0; i < 4; i++) {
        const cJSON *sec = cJSON_GetArrayItem(sections, i);
        CHECK(strcmp(cJSON_GetObjectItem(sec, "name")->valuestring,
                     k_names[i]) == 0,
              "caps", "section %d name", i);
        CHECK(strcmp(cJSON_GetObjectItem(sec, "kind")->valuestring,
                     k_kinds[i]) == 0,
              "caps", "section %d kind", i);
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(sec, "configurable")), "caps",
              "section %d configurable", i);
        CHECK(cJSON_IsBool(cJSON_GetObjectItem(sec, "scannable")), "caps",
              "section %d scannable present", i);
    }

    /* NE101 rules (doc 8): wifi lacks bssid, cellular lacks roaming,
     * webhook lacks secret; only wifi is scannable. */
    const cJSON *wifi = cJSON_GetArrayItem(sections, 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(wifi, "scannable")), "caps",
          "wifi scannable with radio");
    const cJSON *wifi_fields = cJSON_GetObjectItem(wifi, "fields");
    CHECK(cJSON_GetArraySize(wifi_fields) == 2, "caps", "wifi fields 2");
    CHECK(strcmp(cJSON_GetArrayItem(wifi_fields, 0)->valuestring, "ssid") ==
              0,
          "caps", "wifi field 0 ssid");

    const cJSON *cellular = cJSON_GetArrayItem(sections, 1);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cellular, "scannable")), "caps",
          "cellular not scannable");
    const cJSON *cell_fields = cJSON_GetObjectItem(cellular, "fields");
    CHECK(cJSON_GetArraySize(cell_fields) == 5, "caps", "cellular fields 5");
    bool has_roaming = false;
    for (int i = 0; i < 5; i++) {
        if (strcmp(cJSON_GetArrayItem(cell_fields, i)->valuestring,
                   "roaming") == 0) {
            has_roaming = true;
        }
    }
    CHECK(!has_roaming, "caps", "NE101 cellular has no roaming");

    const cJSON *mqtt = cJSON_GetArrayItem(sections, 2);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(mqtt, "fields")) == 9,
          "caps", "mqtt fields 9");

    const cJSON *webhook = cJSON_GetArrayItem(sections, 3);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(webhook, "fields")) == 3,
          "caps", "webhook fields 3");

    CHECK(cJSON_IsNull(cJSON_GetObjectItem(
              cJSON_GetObjectItem(data, "current"), "network")),
          "caps", "current.network null when unconfigured");
    cJSON_Delete(root);
}

static void test_capabilities_current_and_scannable(void)
{
    char scratch[2048];
    char resp[RESP_CAP];
    ct_prov_capabilities_response(8, false, "wifi", scratch, sizeof(scratch),
                                  resp, sizeof(resp));
    cJSON *root = cJSON_Parse(resp);
    CHECK(root != NULL, "caps-current", "parse");
    if (root == NULL) {
        return;
    }
    const cJSON *sections =
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "sections");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(sections, 0),
                                            "scannable")),
          "caps-current", "cat1 variant reports wifi not scannable");
    const cJSON *network = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "current"),
        "network");
    CHECK(network != NULL && strcmp(network->valuestring, "wifi") == 0,
          "caps-current", "current.network echoes wifi");
    cJSON_Delete(root);
}

/* ---- config.get -------------------------------------------------------- */

/* Fixture store: name -> value strings; numerics kept as strings too. */
#define FIX_MAX 24
typedef struct {
    const char *key;
    const char *value;
} kv_t;

static kv_t g_fix[FIX_MAX];
static int g_fix_n;

static void fix_reset(void)
{
    g_fix_n = 0;
    memset(g_fix, 0, sizeof(g_fix));
}

static void fix_set(const char *key, const char *value)
{
    for (int i = 0; i < g_fix_n; i++) {
        if (strcmp(g_fix[i].key, key) == 0) {
            g_fix[i].value = value;
            return;
        }
    }
    if (g_fix_n < FIX_MAX) {
        g_fix[g_fix_n].key = key;
        g_fix[g_fix_n].value = value;
        g_fix_n++;
    }
}

static const char *fix_get(const char *key)
{
    for (int i = 0; i < g_fix_n; i++) {
        if (strcmp(g_fix[i].key, key) == 0) {
            return g_fix[i].value;
        }
    }
    return NULL;
}

static void rd_str(void *ctx, const char *key, char *out, size_t cap)
{
    (void)ctx;
    const char *v = fix_get(key);
    snprintf(out, cap, "%s", v != NULL ? v : "");
}

static bool rd_u8(void *ctx, const char *key, uint8_t *out)
{
    (void)ctx;
    const char *v = fix_get(key);
    if (v == NULL) {
        return false;
    }
    *out = (uint8_t)atoi(v);
    return true;
}

static bool rd_u32(void *ctx, const char *key, uint32_t *out)
{
    (void)ctx;
    const char *v = fix_get(key);
    if (v == NULL) {
        return false;
    }
    *out = (uint32_t)strtoul(v, NULL, 10);
    return true;
}

static const ct_prov_reader_t k_reader = {
    .ctx = NULL, .read_str = rd_str, .read_u8 = rd_u8, .read_u32 = rd_u32};

static void test_config_get_empty_and_masked(void)
{
    char scratch[2048];
    char resp[RESP_CAP];

    /* Fresh device: nothing stored -> empty object. */
    fix_reset();
    ct_prov_config_get_response(9, &k_reader, scratch, sizeof(scratch), resp,
                                sizeof(resp));
    cJSON *root = cJSON_Parse(resp);
    CHECK(root != NULL, "get-empty", "parse");
    if (root != NULL) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        CHECK(data != NULL && cJSON_GetArraySize(data) == 0, "get-empty",
              "no sections on a fresh device");
        cJSON_Delete(root);
    }

    /* Configured wifi + mqtt: passwords masked, sections keyed by identity. */
    fix_reset();
    fix_set("wifi:ssid", "Office-2.4G");
    fix_set("wifi:password", "example-password");
    fix_set("mqtt:host", "mqtt.example.com");
    fix_set("mqtt:port", "8883");
    fix_set("mqtt:enable", "1");
    fix_set("mqtt:tlsEnable", "1");
    ct_prov_config_get_response(10, &k_reader, scratch, sizeof(scratch), resp,
                                sizeof(resp));
    root = cJSON_Parse(resp);
    CHECK(root != NULL, "get-masked", "parse");
    if (root == NULL) {
        return;
    }
    const cJSON *data = cJSON_GetObjectItem(root, "data");
    CHECK(cJSON_GetArraySize(data) == 2, "get-masked", "wifi + mqtt emitted");
    const cJSON *wifi = cJSON_GetObjectItem(data, "wifi");
    CHECK(strcmp(cJSON_GetObjectItem(wifi, "ssid")->valuestring,
                 "Office-2.4G") == 0,
          "get-masked", "ssid read back");
    CHECK(strcmp(cJSON_GetObjectItem(wifi, "password")->valuestring,
                 "********") == 0,
          "get-masked", "password masked");
    const cJSON *mqtt = cJSON_GetObjectItem(data, "mqtt");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(mqtt, "enable")), "get-masked",
          "mqtt enable true");
    CHECK(cJSON_GetObjectItem(mqtt, "port")->valueint == 8883, "get-masked",
          "mqtt port read back");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(mqtt, "tls")), "get-masked",
          "mqtt tls true");
    CHECK(cJSON_GetObjectItem(mqtt, "password") == NULL, "get-masked",
          "unset mqtt password omitted, not empty-masked");
    cJSON_Delete(root);

    /* Open network: no password stored -> no password member at all. */
    fix_reset();
    fix_set("wifi:ssid", "OpenNet");
    ct_prov_config_get_response(11, &k_reader, scratch, sizeof(scratch), resp,
                                sizeof(resp));
    root = cJSON_Parse(resp);
    CHECK(root != NULL, "get-open", "parse");
    if (root != NULL) {
        const cJSON *wifi = cJSON_GetObjectItem(
            cJSON_GetObjectItem(root, "data"), "wifi");
        CHECK(cJSON_GetObjectItem(wifi, "password") == NULL, "get-open",
              "open network has no password member");
        cJSON_Delete(root);
    }
}

int main(void)
{
    test_capabilities_shape();
    test_capabilities_current_and_scannable();
    test_config_get_empty_and_masked();
    printf("caps/get: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
