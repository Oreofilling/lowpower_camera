/*
 * Golden-vector driven host tests for the CT-Prov fragment layer.
 * Mirrors the Dart test contract in neolink ct_prov_core/test/fragment_test.dart:
 * reassembly, rejection tokens, hanging buffer, and byte-exact re-slicing.
 * The vectors header is generated from the Dart source of truth.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ct_prov_fragment.h"
#include "ct_prov_vectors.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ctx, fmt, ...)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL [%s] %s:%d: " fmt "\n", ctx, __func__, __LINE__,       \
                   ##__VA_ARGS__);                                              \
        }                                                                       \
    } while (0)

static bool kind_from_token(const char *token, ct_prov_packet_kind_t *out)
{
    const ct_prov_packet_kind_t cands[4] = {
        CT_PROV_KIND_REQUEST, CT_PROV_KIND_RESPONSE, CT_PROV_KIND_EVENT,
        CT_PROV_KIND_PROTOCOL_ERROR,
    };
    for (size_t i = 0; i < 4; i++) {
        const char *name = ct_prov_kind_name(cands[i]);
        if (name != NULL && strcmp(name, token) == 0) {
            *out = cands[i];
            return true;
        }
    }
    return false;
}

static void test_reassemble(void)
{
    for (size_t i = 0; i < CT_PROV_FRAGMENT_VECTOR_COUNT; i++) {
        const ct_prov_fragment_vector_t *v = &k_ct_prov_fragment_vectors[i];
        if (v->rejected) {
            continue;
        }
        ct_prov_assembler_t a;
        CHECK(ct_prov_assembler_init(&a, v->mtu) == 0, v->id, "init mtu %u", v->mtu);
        ct_prov_feed_state_t st = CT_PROV_FEED_ASSEMBLING;
        for (size_t f = 0; f < v->frame_count; f++) {
            st = ct_prov_assembler_feed(&a, v->frames[f].data, v->frames[f].len, NULL);
            if (f < v->frame_count - 1) {
                CHECK(st == CT_PROV_FEED_ASSEMBLING, v->id,
                      "frame %zu should be assembling", f);
            }
        }
        CHECK(st == CT_PROV_FEED_ASSEMBLED, v->id, "last frame should assemble");
        if (st != CT_PROV_FEED_ASSEMBLED) {
            continue;
        }
        ct_prov_packet_kind_t want;
        if (kind_from_token(v->packet_kind, &want)) {
            CHECK(ct_prov_assembler_kind(&a) == want, v->id, "packet kind");
        }
        CHECK(ct_prov_assembler_message_id(&a) == v->message_id, v->id,
              "message_id");
        size_t len = 0;
        const uint8_t *payload = ct_prov_assembler_payload(&a, &len);
        CHECK(len == strlen(v->json_text), v->id, "payload len %zu want %zu",
              len, strlen(v->json_text));
        CHECK(len == strlen(v->json_text) &&
                  memcmp(payload, v->json_text, len) == 0,
              v->id, "payload bytes differ from golden json");
        CHECK(!ct_prov_assembler_has_partial(&a), v->id,
              "buffer must be clear after assembly");
    }
}

static void test_reject(void)
{
    for (size_t i = 0; i < CT_PROV_FRAGMENT_VECTOR_COUNT; i++) {
        const ct_prov_fragment_vector_t *v = &k_ct_prov_fragment_vectors[i];
        if (!v->rejected || strcmp(v->reason, "incomplete") == 0) {
            continue;
        }
        ct_prov_assembler_t a;
        CHECK(ct_prov_assembler_init(&a, v->mtu) == 0, v->id, "init");
        bool rejected = false;
        for (size_t f = 0; f < v->frame_count && !rejected; f++) {
            ct_prov_reject_reason_t why;
            ct_prov_feed_state_t st =
                ct_prov_assembler_feed(&a, v->frames[f].data, v->frames[f].len, &why);
            if (st == CT_PROV_FEED_REJECTED) {
                const char *token = ct_prov_reject_name(why);
                CHECK(token != NULL && strcmp(token, v->reason) == 0, v->id,
                      "reason %s want %s", token ? token : "(null)", v->reason);
                rejected = true;
            }
        }
        CHECK(rejected, v->id, "must be rejected");
        CHECK(!ct_prov_assembler_has_partial(&a), v->id,
              "buffer must clear after rejection");
    }
}

static void test_hanging_buffer(void)
{
    for (size_t i = 0; i < CT_PROV_FRAGMENT_VECTOR_COUNT; i++) {
        const ct_prov_fragment_vector_t *v = &k_ct_prov_fragment_vectors[i];
        if (strcmp(v->reason, "incomplete") != 0) {
            continue;
        }
        ct_prov_assembler_t a;
        CHECK(ct_prov_assembler_init(&a, v->mtu) == 0, v->id, "init");
        for (size_t f = 0; f < v->frame_count; f++) {
            ct_prov_feed_state_t st =
                ct_prov_assembler_feed(&a, v->frames[f].data, v->frames[f].len, NULL);
            CHECK(st == CT_PROV_FEED_ASSEMBLING, v->id,
                  "frame %zu must not complete or reject", f);
        }
        CHECK(ct_prov_assembler_has_partial(&a), v->id,
              "assembly must hang waiting for the final fragment");
    }
}

static void test_slice(void)
{
    for (size_t i = 0; i < CT_PROV_FRAGMENT_VECTOR_COUNT; i++) {
        const ct_prov_fragment_vector_t *v = &k_ct_prov_fragment_vectors[i];
        if (v->rejected) {
            continue;
        }
        ct_prov_packet_kind_t kind;
        CHECK(kind_from_token(v->packet_kind, &kind), v->id, "kind token");
        const size_t payload_len = strlen(v->json_text);
        const unsigned payload_max = v->mtu - CT_PROV_ATT_HEADER_SIZE -
                                     CT_PROV_FRAGMENT_HEADER_SIZE;
        CHECK(ct_prov_slice_frame_count(payload_len, v->mtu) == v->frame_count,
              v->id, "frame count mismatch");
        const size_t stride = payload_max + CT_PROV_FRAGMENT_HEADER_SIZE;
        uint8_t *frames = malloc(v->frame_count * stride);
        unsigned *lens = malloc(v->frame_count * sizeof(unsigned));
        CHECK(frames != NULL && lens != NULL, v->id, "oom");
        if (frames == NULL || lens == NULL) {
            free(frames);
            free(lens);
            continue;
        }
        const size_t count = ct_prov_slice((const uint8_t *)v->json_text,
                                           payload_len, v->mtu, kind,
                                           v->message_id, frames, stride,
                                           lens, v->frame_count);
        CHECK(count == v->frame_count, v->id, "slice count %zu want %zu", count,
              v->frame_count);
        for (size_t f = 0; f < count && f < v->frame_count; f++) {
            const size_t want_len = v->frames[f].len;
            CHECK(lens[f] == want_len, v->id, "frame %zu len %u want %zu", f,
                  lens[f], want_len);
            CHECK(lens[f] == want_len &&
                      memcmp(frames + f * stride, v->frames[f].data,
                             want_len) == 0,
                  v->id, "frame %zu bytes differ from golden", f);
        }
        free(frames);
        free(lens);
    }
}

static void test_slice_guards(void)
{
    uint8_t big[4302];
    memset(big, 'A', sizeof(big));
    uint8_t frames[16];
    CHECK(ct_prov_slice(big, sizeof(big), 247, CT_PROV_KIND_REQUEST, 1, frames,
                        247 - 8, NULL, 16) == 0,
          "slice-guard", "4302B over the 4096 cap must be refused");
    CHECK(ct_prov_slice(big, 3826, 23, CT_PROV_KIND_REQUEST, 1, frames, 15,
                        NULL, 16) == 0,
          "slice-guard", "3826B at MTU 23 needs 256 fragments, must be refused");
    CHECK(ct_prov_slice(big, 1, 8, CT_PROV_KIND_REQUEST, 1, frames, 1, NULL,
                        16) == 0,
          "slice-guard", "mtu 8 has no payload room, must be refused");
    CHECK(ct_prov_slice(big, 0, 23, CT_PROV_KIND_REQUEST, 1, frames, 15, NULL,
                        16) == 0,
          "slice-guard", "empty payload must be refused");
}

static void test_message_id_sequencer(void)
{
    uint8_t seq = 254;
    uint8_t seen[4];
    for (int i = 0; i < 4; i++) {
        seen[i] = ct_prov_next_message_id(&seq);
    }
    CHECK(seen[0] == 254 && seen[1] == 255 && seen[2] == 0 && seen[3] == 1,
          "sequencer", "wrap 254,255,0,1 got %u,%u,%u,%u", seen[0], seen[1],
          seen[2], seen[3]);
}

static void test_init_guard(void)
{
    ct_prov_assembler_t a;
    CHECK(ct_prov_assembler_init(&a, 8) == -1, "init-guard",
          "mtu 8 must be refused");
}

int main(void)
{
    test_reassemble();
    test_reject();
    test_hanging_buffer();
    test_slice();
    test_slice_guards();
    test_message_id_sequencer();
    test_init_guard();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
