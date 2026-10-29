/*
 * CT-Prov/1 fragmentation layer implementation (protocol doc section 5).
 * Rule order mirrors the Dart reference in neolink ct_prov_core exactly:
 * header checks first, then accumulation, then the 4096 cap, then the
 * final-frame decision. Rejection tokens must stay identical across ends.
 */
#include "ct_prov_fragment.h"

#include <string.h>

const char *ct_prov_kind_name(ct_prov_packet_kind_t kind)
{
    switch (kind) {
    case CT_PROV_KIND_REQUEST:
        return "request";
    case CT_PROV_KIND_RESPONSE:
        return "response";
    case CT_PROV_KIND_EVENT:
        return "event";
    case CT_PROV_KIND_PROTOCOL_ERROR:
        return "protocolError";
    default:
        return NULL;
    }
}

bool ct_prov_kind_from_byte(uint8_t byte, ct_prov_packet_kind_t *out)
{
    switch (byte) {
    case CT_PROV_KIND_REQUEST:
    case CT_PROV_KIND_RESPONSE:
    case CT_PROV_KIND_EVENT:
    case CT_PROV_KIND_PROTOCOL_ERROR:
        if (out != NULL) {
            *out = (ct_prov_packet_kind_t)byte;
        }
        return true;
    default:
        return false;
    }
}

static const char *k_reject_names[CT_PROV_REJECT__COUNT] = {
    "malformed_frame",      "bad_magic",        "unknown_packet_kind",
    "out_of_order",         "duplicate_fragment", "header_changed",
    "oversized",
};

const char *ct_prov_reject_name(ct_prov_reject_reason_t reason)
{
    if ((unsigned)reason >= CT_PROV_REJECT__COUNT) {
        return NULL;
    }
    return k_reject_names[reason];
}

int ct_prov_assembler_init(ct_prov_assembler_t *a, unsigned mtu)
{
    if (mtu < CT_PROV_ATT_HEADER_SIZE + CT_PROV_FRAGMENT_HEADER_SIZE + 1) {
        return -1;
    }
    memset(a, 0, sizeof(*a));
    a->mtu = mtu;
    a->payload_max = mtu - CT_PROV_ATT_HEADER_SIZE - CT_PROV_FRAGMENT_HEADER_SIZE;
    return 0;
}

/* Clear the in-progress assembly. kind/message_id survive so the completed
 * packet accessors stay valid until the next feed starts a new packet. */
static void assembly_reset(ct_prov_assembler_t *a)
{
    a->assembling = false;
    a->fragment_count = 0;
    a->next_index = 0;
    a->received = 0;
}

static ct_prov_feed_state_t reject(ct_prov_assembler_t *a,
                                   ct_prov_reject_reason_t reason,
                                   ct_prov_reject_reason_t *out)
{
    assembly_reset(a);
    if (out != NULL) {
        *out = reason;
    }
    return CT_PROV_FEED_REJECTED;
}

ct_prov_feed_state_t ct_prov_assembler_feed(ct_prov_assembler_t *a,
                                            const uint8_t *frame, size_t len,
                                            ct_prov_reject_reason_t *reason)
{
    if (len < CT_PROV_FRAGMENT_HEADER_SIZE) {
        return reject(a, CT_PROV_REJECT_MALFORMED_FRAME, reason);
    }

    const uint8_t magic = frame[0];
    const uint8_t kind_byte = frame[1];
    const uint8_t message_id = frame[2];
    const uint8_t index = frame[3];
    const uint8_t count = frame[4];
    const uint8_t *payload = frame + CT_PROV_FRAGMENT_HEADER_SIZE;
    const size_t payload_len = len - CT_PROV_FRAGMENT_HEADER_SIZE;

    if (magic != CT_PROV_FRAGMENT_MAGIC) {
        return reject(a, CT_PROV_REJECT_BAD_MAGIC, reason);
    }
    ct_prov_packet_kind_t kind;
    if (!ct_prov_kind_from_byte(kind_byte, &kind)) {
        return reject(a, CT_PROV_REJECT_UNKNOWN_PACKET_KIND, reason);
    }

    if (!a->assembling) {
        if (count < 1) {
            return reject(a, CT_PROV_REJECT_MALFORMED_FRAME, reason);
        }
        if (index != 0) {
            return reject(a, CT_PROV_REJECT_OUT_OF_ORDER, reason);
        }
        a->assembling = true;
        a->kind = kind;
        a->message_id = message_id;
        a->fragment_count = count;
        a->next_index = 0;
    } else if (kind_byte != (uint8_t)a->kind || message_id != a->message_id ||
               count != a->fragment_count) {
        return reject(a, CT_PROV_REJECT_HEADER_CHANGED, reason);
    } else if (index < a->next_index) {
        return reject(a, CT_PROV_REJECT_DUPLICATE_FRAGMENT, reason);
    } else if (index > a->next_index) {
        return reject(a, CT_PROV_REJECT_OUT_OF_ORDER, reason);
    }

    /* Cap check must precede the copy: a hostile frame may otherwise write
     * past buf[4096] before the overflow is discovered. */
    if (a->received + payload_len > CT_PROV_MAX_LOGICAL_PACKET) {
        return reject(a, CT_PROV_REJECT_OVERSIZED, reason);
    }
    memcpy(a->buf + a->received, payload, payload_len);
    a->received += payload_len;

    if (index != (uint8_t)(a->fragment_count - 1)) {
        /* Non-final fragments must carry exactly payload_max bytes. */
        if (payload_len != a->payload_max) {
            return reject(a, CT_PROV_REJECT_MALFORMED_FRAME, reason);
        }
        a->next_index = index + 1;
        return CT_PROV_FEED_ASSEMBLING;
    }

    a->packet_len = a->received;
    assembly_reset(a);
    return CT_PROV_FEED_ASSEMBLED;
}

ct_prov_packet_kind_t ct_prov_assembler_kind(const ct_prov_assembler_t *a)
{
    return a->kind;
}

uint8_t ct_prov_assembler_message_id(const ct_prov_assembler_t *a)
{
    return a->message_id;
}

const uint8_t *ct_prov_assembler_payload(const ct_prov_assembler_t *a,
                                         size_t *len)
{
    if (len != NULL) {
        *len = a->packet_len;
    }
    return a->buf;
}

bool ct_prov_assembler_has_partial(const ct_prov_assembler_t *a)
{
    return a->assembling;
}

size_t ct_prov_slice_frame_count(size_t payload_len, unsigned mtu)
{
    if (payload_len == 0 ||
        payload_len > CT_PROV_MAX_LOGICAL_PACKET ||
        mtu < CT_PROV_ATT_HEADER_SIZE + CT_PROV_FRAGMENT_HEADER_SIZE + 1) {
        return 0;
    }
    const unsigned payload_max =
        mtu - CT_PROV_ATT_HEADER_SIZE - CT_PROV_FRAGMENT_HEADER_SIZE;
    const size_t count = (payload_len + payload_max - 1) / payload_max;
    return count > CT_PROV_MAX_FRAGMENTS ? 0 : count;
}

size_t ct_prov_slice(const uint8_t *payload, size_t len, unsigned mtu,
                     ct_prov_packet_kind_t kind, uint8_t message_id,
                     uint8_t *frames, unsigned stride, unsigned *frame_lens,
                     size_t frames_cap)
{
    const size_t count = ct_prov_slice_frame_count(len, mtu);
    const unsigned payload_max =
        mtu - CT_PROV_ATT_HEADER_SIZE - CT_PROV_FRAGMENT_HEADER_SIZE;
    if (count == 0 || frames == NULL ||
        stride < payload_max + CT_PROV_FRAGMENT_HEADER_SIZE ||
        frames_cap < count) {
        return 0;
    }

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        const size_t chunk =
            len - off > payload_max ? payload_max : len - off;
        uint8_t *frame = frames + i * (size_t)stride;
        frame[0] = CT_PROV_FRAGMENT_MAGIC;
        frame[1] = (uint8_t)kind;
        frame[2] = message_id;
        frame[3] = (uint8_t)i;
        frame[4] = (uint8_t)count;
        memcpy(frame + CT_PROV_FRAGMENT_HEADER_SIZE, payload + off, chunk);
        if (frame_lens != NULL) {
            frame_lens[i] = CT_PROV_FRAGMENT_HEADER_SIZE + (unsigned)chunk;
        }
        off += chunk;
    }
    return count;
}

uint8_t ct_prov_next_message_id(uint8_t *sequencer)
{
    const uint8_t current = *sequencer;
    *sequencer = (uint8_t)(current + 1);
    return current;
}
