/*
 * Golden-vector driven host tests for the CT-Prov envelope layer.
 * Mirrors the Dart test contract in neolink ct_prov_core/test/envelope_test.dart:
 * rejection tokens for invalid envelopes, byte-exact decode/re-encode roundtrips
 * for valid ones, plus encoder guards and rejection-order unit cases.
 * The vectors header is generated from the Dart source of truth.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ct_prov_envelope.h"
#include "ct_prov_fragment.h" /* CT_PROV_MAX_LOGICAL_PACKET */
#include "ct_prov_vectors.h"

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

static void expect_reject(const char *ctx, const char *json,
                          const char *want_token)
{
    ct_prov_envelope_t e;
    ct_prov_envelope_error_t err = CT_PROV_ENV_ERR_MALFORMED;
    const int rc =
        ct_prov_envelope_decode((const uint8_t *)json, strlen(json), &e, &err);
    const char *token = ct_prov_envelope_error_name(err);
    CHECK(rc == -1, ctx, "must be rejected");
    CHECK(rc == -1 && token != NULL && strcmp(token, want_token) == 0, ctx,
          "reason %s want %s", token ? token : "(null)", want_token);
}

static void test_vectors_invalid(void)
{
    for (size_t i = 0; i < CT_PROV_ENVELOPE_VECTOR_COUNT; i++) {
        const ct_prov_envelope_vector_t *v = &k_ct_prov_envelope_vectors[i];
        if (v->valid) {
            continue;
        }
        expect_reject(v->id, v->json_text, v->invalid_reason);
    }
}

/* Expected dispatch for a valid vector, inferred from the golden text the
 * same way the Dart test table classifies its rows. */
static ct_prov_envelope_kind_t kind_of(const char *json)
{
    if (strstr(json, "\"event\"") != NULL) {
        return CT_PROV_ENV_EVENT;
    }
    if (strstr(json, "\"ok\"") != NULL) {
        return CT_PROV_ENV_RESPONSE;
    }
    return CT_PROV_ENV_REQUEST;
}

static void test_vectors_roundtrip(void)
{
    char buf[CT_PROV_MAX_LOGICAL_PACKET + 1];
    for (size_t i = 0; i < CT_PROV_ENVELOPE_VECTOR_COUNT; i++) {
        const ct_prov_envelope_vector_t *v = &k_ct_prov_envelope_vectors[i];
        if (!v->valid) {
            continue;
        }
        ct_prov_envelope_t e;
        ct_prov_envelope_error_t err;
        CHECK(ct_prov_envelope_decode((const uint8_t *)v->json_text,
                                      strlen(v->json_text), &e,
                                      &err) == 0,
              v->id, "decode");
        if (e.op == NULL && e.event == NULL) {
            /* Decode failed and nothing was allocated; skip the roundtrip. */
            continue;
        }
        CHECK(e.kind == kind_of(v->json_text), v->id, "dispatch kind");

        size_t n = 0;
        if (e.kind == CT_PROV_ENV_EVENT) {
            n = ct_prov_envelope_encode_event(e.seq, e.event, e.data_json, buf,
                                              sizeof(buf));
        } else if (e.kind == CT_PROV_ENV_RESPONSE) {
            n = ct_prov_envelope_encode_response(e.id, e.op, e.ok,
                                                 e.data_json, e.error_code,
                                                 e.error_message, buf,
                                                 sizeof(buf));
        } else {
            n = ct_prov_envelope_encode_request(e.id, e.op, e.data_json, buf,
                                                sizeof(buf));
        }
        const size_t want = strlen(v->json_text);
        CHECK(n == want, v->id, "re-encode len %zu want %zu", n, want);
        CHECK(n == want && memcmp(buf, v->json_text, want) == 0, v->id,
              "re-encode bytes differ from golden json");

        ct_prov_envelope_t again;
        CHECK(ct_prov_envelope_decode((const uint8_t *)buf, n, &again,
                                      NULL) == 0,
              v->id, "canonical form must re-decode");
        ct_prov_envelope_free(&again);
        ct_prov_envelope_free(&e);
    }
}

static void test_rejection_order_units(void)
{
    /* bom wins even when the rest of the input is broken too */
    expect_reject("unit-bom", "\357\273\277[]", "bom");
    /* invalid_utf8 is checked before not_object */
    expect_reject("unit-utf8", "\xFF\xFE", "invalid_utf8");
    /* not_object before malformed/trailing */
    expect_reject("unit-not-object", "[]", "not_object");
    expect_reject("unit-not-object-num", "42", "not_object");
    /* structural malformed: the object never closes */
    expect_reject("unit-malformed", "{\"op\":", "malformed");
    /* braces balance but the JSON inside is invalid */
    expect_reject("unit-json-malformed", "{\"id\":1,\"data\":{,}}", "malformed");
    /* trailing garbage after a complete object */
    expect_reject("unit-trailing", "{} x", "trailing_garbage");
    expect_reject("unit-trailing-num", "{}1", "trailing_garbage");
    /* float_int fires before the shape rules, on id and seq alike */
    expect_reject("unit-float-id", "{\"id\":1.0,\"op\":\"x\",\"data\":{}}",
                  "float_int");
    expect_reject("unit-exp-id", "{\"id\":1e18,\"op\":\"x\",\"data\":{}}",
                  "float_int");
    expect_reject("unit-float-seq", "{\"seq\":2.5,\"event\":\"e\",\"data\":{}}",
                  "float_int");
    /* integer literal beyond int64 decodes as double in Dart: float_int */
    expect_reject("unit-overflow-id",
                  "{\"id\":99999999999999999999,\"op\":\"x\",\"data\":{}}",
                  "float_int");
    /* shape rules */
    expect_reject("unit-shape-no-op", "{\"id\":1,\"data\":{}}", "invalid_shape");
    expect_reject("unit-shape-neg-id", "{\"id\":-1,\"op\":\"x\",\"data\":{}}",
                  "invalid_shape");
    expect_reject("unit-shape-data-missing", "{\"id\":1,\"op\":\"x\"}",
                  "invalid_shape");
    expect_reject("unit-shape-resp-op", "{\"id\":1,\"op\":\"x\",\"ok\":true}",
                  "invalid_shape");
    expect_reject("unit-shape-ok-type",
                  "{\"id\":1,\"op\":\"x.result\",\"ok\":\"yes\"}",
                  "invalid_shape");
    expect_reject("unit-shape-data-array",
                  "{\"id\":1,\"op\":\"x.result\",\"ok\":true,\"data\":[]}",
                  "invalid_shape");
    expect_reject("unit-shape-err-obj",
                  "{\"id\":1,\"op\":\"x.result\",\"ok\":false,\"error\":{}}",
                  "invalid_shape");
    expect_reject("unit-shape-event-seq", "{\"seq\":-1,\"event\":\"e\",\"data\":{}}",
                  "invalid_shape");
}

static void test_encode_guards(void)
{
    char buf[64];
    CHECK(ct_prov_envelope_encode_response(1, "x.result", true, NULL, "code",
                                           "msg", buf, sizeof(buf)) == 0,
          "guard", "ok response with error fields must be refused");
    CHECK(ct_prov_envelope_encode_response(1, "x.result", false, NULL, NULL,
                                           NULL, buf, sizeof(buf)) == 0,
          "guard", "not-ok response without error fields must be refused");
    CHECK(ct_prov_envelope_encode_request(1, NULL, "{}", buf, sizeof(buf)) == 0,
          "guard", "null op must be refused");
    CHECK(ct_prov_envelope_encode_request(1, "x", "{}", buf, 4) == 0,
          "guard", "too-small buffer must be refused");
    CHECK(ct_prov_envelope_encode_request(1, "x", "{}", buf, sizeof(buf)) > 0,
          "guard", "minimal request must encode");
}

int main(void)
{
    test_vectors_invalid();
    test_vectors_roundtrip();
    test_rejection_order_units();
    test_encode_guards();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
