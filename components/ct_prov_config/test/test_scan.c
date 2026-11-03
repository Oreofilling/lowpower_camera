/*
 * Host tests for the F2 communication.scan handler: request parsing rules
 * and cursor paging over a caller-built item array (protocol doc section 9).
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

/* ---- parse ------------------------------------------------------------- */

static void test_scan_parse(void)
{
    char section[16];
    bool has_cursor = true;
    size_t cursor = 999;

    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\"}", section, sizeof(section),
                             &has_cursor, &cursor) == 0,
          "parse", "minimal ok");
    CHECK(strcmp(section, "wifi") == 0, "parse", "section");
    CHECK(!has_cursor, "parse", "no cursor flag");
    CHECK(cursor == 0, "parse", "cursor zeroed");

    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\",\"cursor\":\"8\"}", section,
                             sizeof(section), &has_cursor, &cursor) == 0,
          "parse", "cursor ok");
    CHECK(has_cursor && cursor == 8, "parse", "cursor value 8");

    /* Empty cursor string counts as absent (final page echo). */
    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\",\"cursor\":\"\"}", section,
                             sizeof(section), &has_cursor, &cursor) == 0,
          "parse", "empty cursor ok");
    CHECK(!has_cursor, "parse", "empty cursor means absent");

    CHECK(ct_prov_scan_parse("{\"cursor\":\"1\"}", section, sizeof(section),
                             &has_cursor, &cursor) == -1,
          "parse", "missing section rejected");
    CHECK(ct_prov_scan_parse("{\"section\":\"\"}", section, sizeof(section),
                             &has_cursor, &cursor) == -1,
          "parse", "empty section rejected");
    CHECK(ct_prov_scan_parse("{\"section\":8}", section, sizeof(section),
                             &has_cursor, &cursor) == -1,
          "parse", "non-string section rejected");
    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\",\"cursor\":\"abc\"}",
                             section, sizeof(section), &has_cursor,
                             &cursor) == -1,
          "parse", "non-numeric cursor rejected");
    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\",\"cursor\":\"-1\"}",
                             section, sizeof(section), &has_cursor,
                             &cursor) == -1,
          "parse", "negative cursor rejected");
    CHECK(ct_prov_scan_parse("{\"section\":\"wifi\",\"cursor\":8}", section,
                             sizeof(section), &has_cursor, &cursor) == -1,
          "parse", "numeric-typed cursor rejected (must be string)");
    CHECK(ct_prov_scan_parse(
              "{\"section\":\"wifi\",\"cursor\":\"99999999999999999999\"}",
              section, sizeof(section), &has_cursor, &cursor) == -1,
          "parse", "overflowing cursor rejected");
    CHECK(ct_prov_scan_parse("{oops", section, sizeof(section), &has_cursor,
                             &cursor) == -1,
          "parse", "bad json rejected");
    CHECK(ct_prov_scan_parse(NULL, section, sizeof(section), &has_cursor,
                             &cursor) == -1,
          "parse", "NULL data rejected");
}

/* ---- page response ------------------------------------------------------ */

#define N_ITEMS 40

static void fill_items(ct_prov_scan_item_t *items, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(items[i].network_id, sizeof(items[i].network_id), "Net-%02zu",
                 i);
        items[i].signal = (int16_t)(-30 - (int)i);
        items[i].channel = (uint8_t)(1 + i % 13);
        items[i].has_channel = true;
    }
}

/* Run one page request; returns the parsed root (caller deletes) and hands
 * the data object out for assertions. */
static cJSON *page_data(bool has_cursor, size_t cursor,
                        const ct_prov_scan_item_t *items, size_t count,
                        char *scratch, size_t scratch_cap, char *resp,
                        const cJSON **data_out)
{
    static int64_t id = 40;
    const size_t n = ct_prov_scan_page_response(
        id++, "wifi", has_cursor, cursor, items, count, scratch, scratch_cap,
        resp, RESP_CAP);
    CHECK(n > 0, "page", "response length %zu", n);
    cJSON *root = cJSON_Parse(resp);
    CHECK(root != NULL, "page", "parse");
    if (root == NULL) {
        *data_out = NULL;
        return NULL;
    }
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(root, "ok")), "page", "ok=true");
    CHECK(strcmp(cJSON_GetObjectItem(root, "op")->valuestring,
                 "communication.scan.result") == 0,
          "page", "op");
    *data_out = cJSON_GetObjectItem(root, "data");
    return root;
}

static void test_scan_single_page(void)
{
    static ct_prov_scan_item_t items[5];
    static char scratch[2048];
    char resp[RESP_CAP];
    fill_items(items, 5);

    const cJSON *data = NULL;
    cJSON *root = page_data(false, 0, items, 5, scratch, sizeof(scratch), resp,
                            &data);
    if (root == NULL) {
        return;
    }
    CHECK(strcmp(cJSON_GetObjectItem(data, "section")->valuestring, "wifi") ==
              0,
          "single", "section echoed");
    const cJSON *j_items = cJSON_GetObjectItem(data, "items");
    CHECK(cJSON_GetArraySize(j_items) == 5, "single", "all 5 items");
    const cJSON *first = cJSON_GetArrayItem(j_items, 0);
    CHECK(strcmp(cJSON_GetObjectItem(first, "network_id")->valuestring,
                 "Net-00") == 0,
          "single", "network_id");
    CHECK(strcmp(cJSON_GetObjectItem(first, "display_name")->valuestring,
                 "Net-00") == 0,
          "single", "display_name mirrors network_id");
    CHECK(cJSON_GetObjectItem(first, "signal")->valueint == -30, "single",
          "signal");
    CHECK(cJSON_GetObjectItem(first, "channel")->valueint == 1, "single",
          "channel present");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(data, "more")), "single",
          "more false");
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data, "cursor")),
                 "") == 0,
          "single", "empty cursor at end");
    cJSON_Delete(root);
}

static void test_scan_paging(void)
{
    static ct_prov_scan_item_t items[N_ITEMS];
    /* Sized like the firmware glue buffer: big enough for a full 32-item
     * page of typical SSIDs; the truncation case has its own test below. */
    static char scratch[4096];
    char resp[RESP_CAP];
    fill_items(items, N_ITEMS);

    /* First page: capped at 32 items, more=true, cursor="32". */
    const cJSON *p1 = NULL;
    cJSON *root = page_data(false, 0, items, N_ITEMS, scratch, sizeof(scratch),
                            resp, &p1);
    if (root == NULL) {
        return;
    }
    const cJSON *j_items = cJSON_GetObjectItem(p1, "items");
    CHECK(cJSON_GetArraySize(j_items) == CT_PROV_SCAN_PAGE_MAX, "paging",
          "page 1 capped at 32");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p1, "more")), "paging",
          "more true on page 1");
    char cur[24];
    snprintf(cur, sizeof(cur), "%d", CT_PROV_SCAN_PAGE_MAX);
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(p1, "cursor")),
                 cur) == 0,
          "paging", "cursor points at index 32");
    cJSON_Delete(root);

    /* Second page from that cursor: the remaining 8, more=false. */
    const cJSON *p2 = NULL;
    root = page_data(true, CT_PROV_SCAN_PAGE_MAX, items, N_ITEMS, scratch,
                     sizeof(scratch), resp, &p2);
    if (root == NULL) {
        return;
    }
    j_items = cJSON_GetObjectItem(p2, "items");
    CHECK(cJSON_GetArraySize(j_items) == N_ITEMS - CT_PROV_SCAN_PAGE_MAX,
          "paging", "page 2 has the remainder");
    const cJSON *first = cJSON_GetArrayItem(j_items, 0);
    CHECK(strcmp(cJSON_GetObjectItem(first, "network_id")->valuestring,
                 "Net-32") == 0,
          "paging", "page 2 starts at Net-32");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p2, "more")), "paging",
          "more false on final page");
    cJSON_Delete(root);
}

static void test_scan_edge_cursors(void)
{
    static ct_prov_scan_item_t items[3];
    static char scratch[512];
    char resp[RESP_CAP];
    fill_items(items, 3);

    /* cursor == count: legal, empty page. */
    const cJSON *empty = NULL;
    cJSON *root = page_data(true, 3, items, 3, scratch, sizeof(scratch), resp,
                            &empty);
    if (root != NULL) {
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(empty, "items")) == 0,
              "edges", "cursor==count yields empty page");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(empty, "more")), "edges",
              "more false");
        cJSON_Delete(root);
    }

    /* cursor > count: stale -> INVALID_ARGUMENT. */
    const size_t n = ct_prov_scan_page_response(
        50, "wifi", true, 4, items, 3, scratch, sizeof(scratch), resp,
        sizeof(resp));
    CHECK(n > 0, "edges", "stale response length");
    cJSON *stale = cJSON_Parse(resp);
    CHECK(stale != NULL, "edges", "stale parse");
    if (stale != NULL) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(stale, "ok")), "edges",
              "stale cursor ok=false");
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                         cJSON_GetObjectItem(stale, "error"), "code")),
                     "INVALID_ARGUMENT") == 0,
              "edges", "stale cursor code");
        cJSON_Delete(stale);
    }
}

static void test_scan_budget_truncation(void)
{
    /* Long SSIDs against a small scratch: the page must stop early, stay
     * valid JSON, and expose a cursor so the client can continue. */
    static ct_prov_scan_item_t items[32];
    static char scratch[768];
    char resp[RESP_CAP];
    for (int i = 0; i < 32; i++) {
        memset(items[i].network_id, 'A' + i % 26,
               sizeof(items[i].network_id) - 1);
        items[i].network_id[sizeof(items[i].network_id) - 1] = '\0';
        items[i].signal = -55;
        items[i].has_channel = false;
    }

    const cJSON *data = NULL;
    cJSON *root = page_data(false, 0, items, 32, scratch, sizeof(scratch),
                            resp, &data);
    if (root == NULL) {
        return;
    }
    const cJSON *j_items = cJSON_GetObjectItem(data, "items");
    const int got = cJSON_GetArraySize(j_items);
    CHECK(got > 0 && got < CT_PROV_SCAN_PAGE_MAX, "budget",
          "page truncated by budget (got %d)", got);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(data, "more")), "budget",
          "more true after truncation");
    const char *cur = cJSON_GetStringValue(cJSON_GetObjectItem(data, "cursor"));
    CHECK(cur != NULL && atoi(cur) == got, "budget",
          "cursor advances by emitted count only");
    const cJSON *last = cJSON_GetArrayItem(j_items, got - 1);
    CHECK(last != NULL && cJSON_GetObjectItem(last, "channel") == NULL,
          "budget", "channel omitted when unknown");
    cJSON_Delete(root);
}

int main(void)
{
    test_scan_parse();
    test_scan_single_page();
    test_scan_paging();
    test_scan_edge_cursors();
    test_scan_budget_truncation();
    printf("scan: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
