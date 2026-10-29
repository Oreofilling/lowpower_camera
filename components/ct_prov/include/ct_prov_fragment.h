/*
 * CT-Prov/1 fragmentation layer (protocol doc section 5).
 * Single-direction reassembler + slicer shared by the device firmware.
 * Byte-exact mirror of the Dart reference in neolink ct_prov_core; both are
 * validated against the same golden vectors (ct_prov_vectors.h).
 */
#ifndef CT_PROV_FRAGMENT_H
#define CT_PROV_FRAGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Section 5 constants. */
#define CT_PROV_FRAGMENT_MAGIC 0xC1
#define CT_PROV_FRAGMENT_HEADER_SIZE 5
#define CT_PROV_ATT_HEADER_SIZE 3 /* ATT write/indication overhead */
#define CT_PROV_MAX_LOGICAL_PACKET 4096
#define CT_PROV_MAX_FRAGMENTS 255

typedef enum {
    CT_PROV_KIND_REQUEST = 0x01,
    CT_PROV_KIND_RESPONSE = 0x02,
    CT_PROV_KIND_EVENT = 0x03,
    CT_PROV_KIND_PROTOCOL_ERROR = 0x7F,
} ct_prov_packet_kind_t;

/* Token names match the golden vector tokens ("request"... "protocolError").
 * Returns NULL for out-of-range values. */
const char *ct_prov_kind_name(ct_prov_packet_kind_t kind);
/* Reverse lookup; returns false for unknown wire bytes. */
bool ct_prov_kind_from_byte(uint8_t byte, ct_prov_packet_kind_t *out);

typedef enum {
    CT_PROV_REJECT_MALFORMED_FRAME = 0,
    CT_PROV_REJECT_BAD_MAGIC,
    CT_PROV_REJECT_UNKNOWN_PACKET_KIND,
    CT_PROV_REJECT_OUT_OF_ORDER,
    CT_PROV_REJECT_DUPLICATE_FRAGMENT,
    CT_PROV_REJECT_HEADER_CHANGED,
    CT_PROV_REJECT_OVERSIZED,
    CT_PROV_REJECT__COUNT
} ct_prov_reject_reason_t;

/* Golden-vector rejection token, or NULL for out-of-range values. */
const char *ct_prov_reject_name(ct_prov_reject_reason_t reason);

typedef enum {
    CT_PROV_FEED_ASSEMBLING = 0,
    CT_PROV_FEED_ASSEMBLED,
    CT_PROV_FEED_REJECTED,
} ct_prov_feed_state_t;

/*
 * Single-direction reassembler. No heap: the packet buffer lives inline, so
 * keep one instance per direction (C2D receive, D2C send) as static storage.
 *
 * Protocol allows only one in-flight assembly per direction; any violation
 * rejects and clears the buffer so the next packet can start cleanly.
 */
typedef struct {
    unsigned mtu;
    unsigned payload_max; /* mtu - ATT header - fragment header */
    /* In-progress assembly state; meaningful while assembling. */
    bool assembling;
    ct_prov_packet_kind_t kind;
    uint8_t message_id;
    uint8_t fragment_count;
    uint8_t next_index;
    size_t received;
    size_t packet_len; /* completed packet length (see accessors) */
    uint8_t buf[CT_PROV_MAX_LOGICAL_PACKET];
} ct_prov_assembler_t;

/* Returns 0 on success, -1 when mtu is too small for a 1-byte payload. */
int ct_prov_assembler_init(ct_prov_assembler_t *a, unsigned mtu);

/*
 * Feed one ATT frame (5-byte header included). The frame is copied, the
 * caller's buffer is not retained. On rejection the buffer is cleared and
 * *reason (optional) receives the cause.
 */
ct_prov_feed_state_t ct_prov_assembler_feed(ct_prov_assembler_t *a,
                                            const uint8_t *frame, size_t len,
                                            ct_prov_reject_reason_t *reason);

/* Completed-packet accessors: valid between ASSEMBLED and the next feed. */
ct_prov_packet_kind_t ct_prov_assembler_kind(const ct_prov_assembler_t *a);
uint8_t ct_prov_assembler_message_id(const ct_prov_assembler_t *a);
const uint8_t *ct_prov_assembler_payload(const ct_prov_assembler_t *a,
                                         size_t *len);
/* True while a partial assembly is waiting for its remaining fragments. */
bool ct_prov_assembler_has_partial(const ct_prov_assembler_t *a);

/*
 * Slicer: splits payload into frames with the 5-byte header prepended.
 * Non-final fragments carry exactly payload_max bytes (section 5).
 *
 * frames is a caller buffer with `stride` bytes per fragment slot (stride
 * must be >= payload_max + CT_PROV_FRAGMENT_HEADER_SIZE); frame_lens
 * (optional, frames_cap entries) receives each frame length. Returns the
 * fragment count, or 0 on invalid arguments: mtu too small, empty payload,
 * payload over CT_PROV_MAX_LOGICAL_PACKET, fragment count over
 * CT_PROV_MAX_FRAGMENTS, stride too small, or frames_cap too small.
 */
size_t ct_prov_slice_frame_count(size_t payload_len, unsigned mtu);
size_t ct_prov_slice(const uint8_t *payload, size_t len, unsigned mtu,
                     ct_prov_packet_kind_t kind, uint8_t message_id,
                     uint8_t *frames, unsigned stride, unsigned *frame_lens,
                     size_t frames_cap);

/* Per-direction message_id sequencer: returns the current value and bumps it
 * modulo 256 (section 5: one sequencer per direction). */
uint8_t ct_prov_next_message_id(uint8_t *sequencer);

#endif /* CT_PROV_FRAGMENT_H */
