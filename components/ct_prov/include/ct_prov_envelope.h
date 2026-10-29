/*
 * CT-Prov/1 envelope layer (protocol doc section 6).
 * Request/response/event decode and encode with the full section 6 rule set:
 * UTF-8 without BOM, top-level object, integers not written as floats, no
 * content after the single value. Byte-exact mirror of the Dart reference in
 * neolink ct_prov_core, validated against the same golden vectors.
 */
#ifndef CT_PROV_ENVELOPE_H
#define CT_PROV_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CT_PROV_ENV_REQUEST = 0,
    CT_PROV_ENV_RESPONSE,
    CT_PROV_ENV_EVENT,
} ct_prov_envelope_kind_t;

/* Decode failure reasons: cross-end stable tokens matching the golden
 * vectors and the Dart reference. */
typedef enum {
    CT_PROV_ENV_ERR_BOM = 0,
    CT_PROV_ENV_ERR_NOT_OBJECT,
    CT_PROV_ENV_ERR_FLOAT_INT,
    CT_PROV_ENV_ERR_TRAILING_GARBAGE,
    CT_PROV_ENV_ERR_MALFORMED,
    CT_PROV_ENV_ERR_INVALID_UTF8,
    CT_PROV_ENV_ERR_INVALID_SHAPE,
    CT_PROV_ENV_ERR__COUNT
} ct_prov_envelope_error_t;

const char *ct_prov_envelope_error_name(ct_prov_envelope_error_t err);

/*
 * Decoded envelope. String fields are heap copies owned by the struct;
 * data_json keeps the RAW original text slice of the data value (verbatim,
 * never re-serialized) so re-encoding is byte-exact. NULL where absent.
 * Zero the struct before decode; free with ct_prov_envelope_free().
 *
 * Known divergences from the Dart reference, all on inputs outside the
 * golden vector set (documented for cross-end review):
 *  - keys written with JSON escapes (e.g. "id") evade the raw-token
 *    integer check here but are normalized by the Dart decoder;
 *  - ids above 2^63 are refused as float_int, matching Dart's fallback.
 */
typedef struct {
    ct_prov_envelope_kind_t kind;
    int64_t id;            /* request, response */
    int64_t seq;           /* event */
    char *op;              /* decoded string (request, response) */
    bool ok;               /* response */
    char *event;           /* decoded string (event) */
    char *data_json;       /* raw value slice; NULL if absent */
    char *error_code;      /* decoded string (response, ok == false) */
    char *error_message;   /* decoded string (response, ok == false) */
} ct_prov_envelope_t;

void ct_prov_envelope_free(ct_prov_envelope_t *e);

/*
 * Decode one complete logical-packet payload (UTF-8 JSON envelope).
 * Returns 0 and fills *out on success; returns -1 and sets *err (optional)
 * on rejection. The input is not retained.
 *
 * Rejection order mirrors the Dart reference: bom, invalid_utf8,
 * not_object, malformed (structure), trailing_garbage, malformed (JSON),
 * float_int (id/seq), invalid_shape.
 */
int ct_prov_envelope_decode(const uint8_t *bytes, size_t len,
                            ct_prov_envelope_t *out,
                            ct_prov_envelope_error_t *err);

/*
 * Encoders: emit compact canonical JSON (key order id,op,data /
 * id,op,ok,data|error / seq,event,data). data_json is spliced verbatim and
 * must be a complete JSON object value; NULL encodes {}.
 *
 * Return the byte length written (excluding the NUL terminator), or 0 on
 * invalid arguments or a too-small buffer. Recommended capacity is
 * CT_PROV_MAX_LOGICAL_PACKET + 1 (protocol payload cap).
 */
size_t ct_prov_envelope_encode_request(int64_t id, const char *op,
                                       const char *data_json, char *buf,
                                       size_t cap);
size_t ct_prov_envelope_encode_response(int64_t id, const char *op, bool ok,
                                        const char *data_json,
                                        const char *error_code,
                                        const char *error_message, char *buf,
                                        size_t cap);
size_t ct_prov_envelope_encode_event(int64_t seq, const char *event,
                                     const char *data_json, char *buf,
                                     size_t cap);

#endif /* CT_PROV_ENVELOPE_H */
