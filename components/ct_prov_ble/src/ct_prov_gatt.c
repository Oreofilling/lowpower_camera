/*
 * CT-Prov/1 GATT server implementation (protocol doc sections 3-6, 11, 13).
 *
 * Pipeline: C2D write -> fragment assembler -> envelope decode -> app
 * handler -> envelope encode -> fragment slicer -> D2C indication, one
 * frame in flight at a time (confirmed via BLE_GAP_EVENT_NOTIFY_TX).
 * Security per doc 4.4: C2D requires an encrypted link.  Bonding with LTK
 * distribution is enabled because OS-level pairing (Windows PairAsync,
 * Android createBond) only reports success when it can store a key; the
 * device also sends the SMP security request on connect so encryption does
 * not depend on the client volunteering.  session.close is answered by the
 * server and followed by a disconnect once the response drains (doc 11).
 */
#include "ct_prov_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* ESP-IDF 5.1's NimBLE examples declare this library entry point locally. */
void ble_store_config_init(void);

#include "ct_prov_adv.h"
#include "ct_prov_fragment.h"

static const char *TAG = "CT_PROV_BLE";

/* Remote user terminated connection; ESP-IDF NimBLE keeps BLE_ERR_*
 * internal, so the well-known HCI code is named here. */
#define CT_PROV_HCI_REMOTE_USER_TERM 0x13

#define CT_PROV_DEFAULT_MTU 23
/* Cap on one sliced D2C payload; keeps the frame buffer bounded when a
 * client negotiates a very large MTU. */
#define CT_PROV_TX_PAYLOAD_MAX 248
/* Doc 13: fragment reassembly timeout. */
#define CT_PROV_REASSEMBLY_TIMEOUT_US (3 * 1000 * 1000)
#define CT_PROV_WATCHDOG_PERIOD_US (500 * 1000)

/*
 * CT-Prov UUIDs 7B1E000X-9D5B-4B5B-8F6D-2A4C0B7E0100.  NimBLE stores
 * 128-bit UUIDs least-significant byte first, so the initializers look
 * reversed from the text form (doc 4.1).  Macro bytes 0-1 map to the last
 * text group (...0b7e0100), bytes 12-13 to the first (7b1e00xx): all four
 * UUIDs share the 0100 tail; the first group carries the per-role id
 * (0001 service, 0002 C2D, 0003 D2C, 0004 META).
 */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
                     0x5b, 0x4b, 0x5b, 0x9d, 0x01, 0x00, 0x1e, 0x7b);
static const ble_uuid128_t s_c2d_uuid =
    BLE_UUID128_INIT(0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
                     0x5b, 0x4b, 0x5b, 0x9d, 0x02, 0x00, 0x1e, 0x7b);
static const ble_uuid128_t s_d2c_uuid =
    BLE_UUID128_INIT(0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
                     0x5b, 0x4b, 0x5b, 0x9d, 0x03, 0x00, 0x1e, 0x7b);
static const ble_uuid128_t s_meta_uuid =
    BLE_UUID128_INIT(0x00, 0x01, 0x7e, 0x0b, 0x4c, 0x2a, 0x6d, 0x8f,
                     0x5b, 0x4b, 0x5b, 0x9d, 0x04, 0x00, 0x1e, 0x7b);

static ct_prov_ble_identity_t s_identity;
static ct_prov_ble_request_cb_t s_req_cb;
static void *s_req_cb_arg;
static uint8_t s_flags = CT_PROV_FLAG_PROVISIONABLE;
static uint8_t s_addr_type;
static bool s_started;
static bool s_stopping;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_d2c_indicate;
static uint16_t s_d2c_handle;
static unsigned s_mtu = CT_PROV_DEFAULT_MTU;

/* Reception. The assembler is touched from the NimBLE host task and from
 * the watchdog timer, so partial-state access takes a critical section. */
static ct_prov_assembler_t s_asm;
static portMUX_TYPE s_asm_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_partial_since_us;
static uint8_t s_rx_frame[CT_PROV_FRAGMENT_HEADER_SIZE + CT_PROV_TX_PAYLOAD_MAX];

/* Transmission: one logical packet, sliced on demand, one indication in
 * flight at a time. */
static uint8_t s_tx_buf[CT_PROV_MAX_LOGICAL_PACKET];
static size_t s_tx_len;
static size_t s_tx_off;
static ct_prov_packet_kind_t s_tx_kind;
static uint8_t s_tx_msg_id;
static uint8_t s_tx_seq; /* D2C message_id, resets per connection (doc 5) */
static bool s_tx_active;
static bool s_close_after_tx;
static uint32_t s_evt_seq; /* protocol.error event seq (doc 6.4) */
static char s_resp_buf[CT_PROV_MAX_LOGICAL_PACKET + 1];

static esp_timer_handle_t s_watchdog;

static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);

/* ---- transmission ---------------------------------------------------- */

static unsigned tx_payload_max(void)
{
    const unsigned by_mtu =
        s_mtu - CT_PROV_ATT_HEADER_SIZE - CT_PROV_FRAGMENT_HEADER_SIZE;
    return by_mtu > CT_PROV_TX_PAYLOAD_MAX ? CT_PROV_TX_PAYLOAD_MAX : by_mtu;
}

/* Effective ATT MTU matching tx_payload_max, for the slicer's validators. */
static unsigned tx_effective_mtu(void)
{
    return tx_payload_max() + CT_PROV_ATT_HEADER_SIZE +
           CT_PROV_FRAGMENT_HEADER_SIZE;
}

static void tx_abort(const char *why)
{
    s_tx_active = false;
    s_close_after_tx = false;
    ESP_LOGW(TAG, "D2C packet dropped: %s", why);
}

static void tx_send_next(void)
{
    const unsigned payload_max = tx_payload_max();
    const size_t remaining = s_tx_len - s_tx_off;
    const size_t chunk =
        remaining > payload_max ? payload_max : remaining;

    uint8_t frame[CT_PROV_FRAGMENT_HEADER_SIZE + CT_PROV_TX_PAYLOAD_MAX];
    frame[0] = CT_PROV_FRAGMENT_MAGIC;
    frame[1] = (uint8_t)s_tx_kind;
    frame[2] = s_tx_msg_id;
    frame[3] = (uint8_t)(s_tx_off / payload_max);
    frame[4] = (uint8_t)((s_tx_len + payload_max - 1) / payload_max);
    memcpy(frame + CT_PROV_FRAGMENT_HEADER_SIZE, s_tx_buf + s_tx_off, chunk);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(
        frame, CT_PROV_FRAGMENT_HEADER_SIZE + chunk);
    if (om == NULL) {
        tx_abort("mbuf pool exhausted");
        return;
    }
    const int rc = ble_gatts_indicate_custom(s_conn_handle, s_d2c_handle, om);
    if (rc != 0) {
        char why[32];
        snprintf(why, sizeof(why), "indicate rc=%d", rc);
        tx_abort(why);
        return;
    }
    s_tx_off += chunk;
}

/*
 * Queue one complete envelope for transmission. Frames are sent one at a
 * time and advanced on each indication confirmation, so the mbuf pool only
 * ever holds a single frame of this service.
 */
static void start_tx(const char *json, size_t len, ct_prov_packet_kind_t kind)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_d2c_indicate) {
        tx_abort("no connection or D2C subscription");
        if (s_close_after_tx) {
            /* session.close response undeliverable: end the session anyway
             * (doc 11 allows client-side disconnect as an equivalent). */
            s_close_after_tx = false;
            ble_gap_terminate(s_conn_handle, CT_PROV_HCI_REMOTE_USER_TERM);
        }
        return;
    }
    if (s_tx_active) {
        tx_abort("previous packet still in flight");
        return;
    }
    if (ct_prov_slice_frame_count(len, tx_effective_mtu()) == 0) {
        tx_abort("packet not slicable at current MTU");
        return;
    }

    memcpy(s_tx_buf, json, len);
    s_tx_len = len;
    s_tx_off = 0;
    s_tx_kind = kind;
    s_tx_msg_id = ct_prov_next_message_id(&s_tx_seq);
    s_tx_active = true;
    tx_send_next();
}

/*
 * Report a framing or envelope failure as a PROTOCOL_ERROR packet (doc 5).
 * Sending is optional per the doc; skipped when nobody would receive it.
 */
static void send_protocol_error(const char *message)
{
    if (message == NULL) {
        message = "malformed";
    }
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_d2c_indicate) {
        ESP_LOGW(TAG, "protocol error (%s) has no D2C subscriber", message);
        return;
    }
    if (s_tx_active) {
        ESP_LOGW(TAG, "protocol error (%s) skipped: TX busy", message);
        return;
    }
    char data[96];
    snprintf(data, sizeof(data),
             "{\"code\":\"INVALID_MESSAGE\",\"message\":\"%.48s\"}", message);
    const size_t n = ct_prov_envelope_encode_event(
        (int64_t)s_evt_seq++, "protocol.error", data, s_resp_buf,
        sizeof(s_resp_buf));
    if (n > 0) {
        start_tx(s_resp_buf, n, CT_PROV_KIND_PROTOCOL_ERROR);
    }
}

/* ---- reception -------------------------------------------------------- */

static void handle_envelope(const uint8_t *payload, size_t len)
{
    ct_prov_envelope_t env;
    ct_prov_envelope_error_t err;
    if (ct_prov_envelope_decode(payload, len, &env, &err) != 0) {
        const char *token = ct_prov_envelope_error_name(err);
        send_protocol_error(token != NULL ? token : "malformed");
        return;
    }
    if (env.kind != CT_PROV_ENV_REQUEST) {
        /* C2D carries requests only (doc 4.1). */
        send_protocol_error("invalid_shape");
        ct_prov_envelope_free(&env);
        return;
    }

    if (strcmp(env.op, "session.close") == 0) {
        /* Doc 11: always succeed, then disconnect once the reply drains. */
        const size_t n = ct_prov_envelope_encode_response(
            env.id, "session.close.result", true, NULL, NULL, NULL,
            s_resp_buf, sizeof(s_resp_buf));
        ct_prov_envelope_free(&env);
        if (n > 0) {
            s_close_after_tx = true;
            start_tx(s_resp_buf, n, CT_PROV_KIND_RESPONSE);
        }
        return;
    }

    size_t n = s_req_cb != NULL ? s_req_cb(s_req_cb_arg, &env, s_resp_buf,
                                           sizeof(s_resp_buf))
                                : 0;
    if (n == 0) {
        /* Guarantee an answer even when the handler stays silent. */
        char op_result[80];
        snprintf(op_result, sizeof(op_result), "%.71s.result", env.op);
        n = ct_prov_envelope_encode_response(
            env.id, op_result, false, NULL, "INTERNAL_ERROR",
            "request handler produced no response", s_resp_buf,
            sizeof(s_resp_buf));
    }
    ct_prov_envelope_free(&env);
    if (n > 0) {
        start_tx(s_resp_buf, n, CT_PROV_KIND_RESPONSE);
    } else {
        ESP_LOGE(TAG, "response encoding failed");
    }
}

/* ---- GATT access ------------------------------------------------------ */

static void maybe_start_security(void);

static int c2d_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* Doc 4.4: writes carrying configuration must ride an encrypted link. */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!desc.sec_state.encrypted) {
        ESP_LOGW(TAG, "C2D write rejected: link not encrypted (doc 4.4)");
        /* Client skipped the doc 3 sequence's pairing step: kick off the
         * security procedure so a retry of this write lands encrypted. */
        maybe_start_security();
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof(s_rx_frame)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_rx_frame, sizeof(s_rx_frame),
                            &copied) != 0 ||
        copied != len) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ct_prov_reject_reason_t reason;
    ct_prov_feed_state_t st;
    taskENTER_CRITICAL(&s_asm_mux);
    st = ct_prov_assembler_feed(&s_asm, s_rx_frame, copied, &reason);
    /* Refresh the hang clock on every fragment so a stalled sender (not a
     * slow one) is who trips the watchdog (doc 13). */
    if (ct_prov_assembler_has_partial(&s_asm)) {
        s_partial_since_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_asm_mux);

    if (st == CT_PROV_FEED_ASSEMBLING) {
        return 0;
    }
    if (st == CT_PROV_FEED_REJECTED) {
        const char *token = ct_prov_reject_name(reason);
        send_protocol_error(token != NULL ? token : "malformed_frame");
        return 0; /* the ATT write itself was well formed */
    }

    size_t payload_len = 0;
    const uint8_t *payload = ct_prov_assembler_payload(&s_asm, &payload_len);
    handle_envelope(payload, payload_len);
    return 0;
}

static int d2c_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Indicate-only characteristic: no read or write semantics exist. */
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static int meta_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* Doc 4.3: readable before pairing, so no encryption check here. */
    uint8_t meta[CT_PROV_META_MAX];
    const int n =
        ct_prov_meta_build(s_identity.device_name, s_flags, meta, sizeof(meta));
    if (n < 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, meta, (uint16_t)n) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Doc 4.1: Write Request only, never Write Without Response. */
                .uuid = &s_c2d_uuid.u,
                .access_cb = c2d_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_d2c_uuid.u,
                .access_cb = d2c_access,
                .val_handle = &s_d2c_handle,
                .flags = BLE_GATT_CHR_F_INDICATE,
            },
            {
                .uuid = &s_meta_uuid.u,
                .access_cb = meta_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

/* ---- GAP -------------------------------------------------------------- */

/* Doc 4.4: credentials need an encrypted link.  The desktop client performs
 * system pairing on a dedicated pre-open connection, before GATT discovery,
 * so the peripheral must volunteer the SMP security request as soon as that
 * connection is established.  This avoids the old discovery/CCCD race while
 * still allowing Windows PairAsync to complete.  The C2D call remains a
 * backstop for clients that skip the documented pairing step.  One shot per
 * connection. */
static bool s_security_req_sent;

static void maybe_start_security(void)
{
    if (s_security_req_sent) {
        return;
    }
    s_security_req_sent = true;
    const int rc = ble_gap_security_initiate(s_conn_handle);
    if (rc != 0) {
        /* Not fatal: an already-encrypted link or a central that pairs on
         * its own makes this redundant; genuine failures still surface at
         * the C2D encryption check. */
        ESP_LOGW(TAG, "security initiate rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "security request sent");
    }
}

/* Reset everything that is scoped to one connection (doc 5: message_id
 * and event seq restart from 0 on each connection). */
static void session_reset(void)
{
    s_mtu = CT_PROV_DEFAULT_MTU;
    taskENTER_CRITICAL(&s_asm_mux);
    ct_prov_assembler_init(&s_asm, s_mtu);
    s_partial_since_us = 0;
    taskEXIT_CRITICAL(&s_asm_mux);
    s_d2c_indicate = false;
    s_security_req_sent = false;
    s_tx_active = false;
    s_close_after_tx = false;
    s_tx_len = 0;
    s_tx_off = 0;
    s_tx_seq = 0;
    s_evt_seq = 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* CONNECT is the canonical point at which the handle becomes valid.
         * ESP-IDF's later LINK_ESTAB extension is emitted only after remote
         * feature discovery, by which time a bonded peer may already have
         * restored encryption and CCCDs; resetting state there loses those
         * events.  LINK_ESTAB is also dispatched while the host lock is held,
         * so initiating security from it can deadlock the NimBLE task. */
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed, status=%d",
                     event->connect.status);
            advertise();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        session_reset();
        ESP_LOGI(TAG, "client connected, handle=%u", s_conn_handle);
        maybe_start_security();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected, reason=%d",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        session_reset();
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_d2c_handle) {
            s_d2c_indicate = event->subscribe.cur_indicate != 0;
            ESP_LOGI(TAG, "D2C indications %s",
                     s_d2c_indicate ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value > 0 ? event->mtu.value : CT_PROV_DEFAULT_MTU;
        ESP_LOGI(TAG, "ATT MTU updated to %u", s_mtu);
        taskENTER_CRITICAL(&s_asm_mux);
        if (ct_prov_assembler_has_partial(&s_asm)) {
            /* Larger MTU changes the non-final frame length, so a partial
             * packet can no longer be completed correctly. */
            ct_prov_assembler_init(&s_asm, s_mtu);
            s_partial_since_us = 0;
            taskEXIT_CRITICAL(&s_asm_mux);
            ESP_LOGW(TAG, "MTU changed mid-assembly; partial packet dropped");
            send_protocol_error("out_of_order");
        } else {
            ct_prov_assembler_init(&s_asm, s_mtu);
            taskEXIT_CRITICAL(&s_asm_mux);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "link encryption %s",
                     desc.sec_state.encrypted ? "on" : "off");
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.attr_handle != s_d2c_handle ||
            !event->notify_tx.indication || !s_tx_active) {
            return 0;
        }
        /* NimBLE reports status=0 synchronously from
         * ble_gatts_indicate_custom(): it means the ATT command was queued,
         * not that the peer confirmed it.  Advancing here recursively calls
         * indicate_custom before the first call has returned and wedges the
         * host.  Only BLE_HS_EDONE is the indication acknowledgment. */
        if (event->notify_tx.status == 0) {
            return 0;
        }
        if (event->notify_tx.status != BLE_HS_EDONE) {
            tx_abort("indication not confirmed");
            return 0;
        }
        if (s_tx_off >= s_tx_len) {
            s_tx_active = false;
            ESP_LOGI(TAG, "D2C packet delivered (%zu bytes)", s_tx_len);
            if (s_close_after_tx) {
                s_close_after_tx = false;
                ble_gap_terminate(s_conn_handle,
                                  CT_PROV_HCI_REMOTE_USER_TERM);
            }
            return 0;
        }
        tx_send_next();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The peer paired although a bond exists (typically the client lost
         * its key, e.g. after a device-side key clear).  Drop our stale
         * copy and let the new procedure run. */
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                                  &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void advertise(void)
{
    if (s_stopping || !s_started) {
        return;
    }

    ct_prov_discovery_t disc = {
        .model_code = s_identity.model_code,
        .flags = s_flags,
    };
    memcpy(disc.mac_suffix, s_identity.mac_suffix, sizeof(disc.mac_suffix));

    uint8_t adv[CT_PROV_ADV_LEN];
    uint8_t scan_rsp[CT_PROV_SCAN_RSP_MAX];
    if (ct_prov_adv_build(&disc, adv, sizeof(adv)) != CT_PROV_ADV_LEN) {
        ESP_LOGE(TAG, "discovery payload rejected; not advertising");
        return;
    }
    const int rsp_len = ct_prov_scan_rsp_build(s_identity.device_name, scan_rsp,
                                               sizeof(scan_rsp));
    if (rsp_len < 0) {
        ESP_LOGE(TAG, "scan response rejected; not advertising");
        return;
    }

    if (ble_gap_adv_set_data(adv, CT_PROV_ADV_LEN) != 0 ||
        ble_gap_adv_rsp_set_data(scan_rsp, rsp_len) != 0) {
        ESP_LOGE(TAG, "setting raw advertising data failed");
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params,
                          gap_event, NULL) != 0) {
        ESP_LOGE(TAG, "advertising start failed");
        return;
    }
    ESP_LOGI(TAG, "advertising %s (model 0x%02X, flags 0x%02X)",
             s_identity.device_name, s_identity.model_code, s_flags);
}

/* ---- stack lifecycle --------------------------------------------------- */

static void watchdog_cb(void *arg)
{
    (void)arg;
    bool timed_out = false;
    taskENTER_CRITICAL(&s_asm_mux);
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        ct_prov_assembler_has_partial(&s_asm) && s_partial_since_us != 0 &&
        esp_timer_get_time() - s_partial_since_us >
            CT_PROV_REASSEMBLY_TIMEOUT_US) {
        ct_prov_assembler_init(&s_asm, s_mtu);
        s_partial_since_us = 0;
        timed_out = true;
    }
    taskEXIT_CRITICAL(&s_asm_mux);
    if (timed_out) {
        /* Doc 5/13: terminate the assembly; the PROTOCOL_ERROR event is
         * optional and is not sent from this timer context. */
        ESP_LOGW(TAG, "fragment reassembly timed out; assembly cleared");
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer address type failed: rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- public API -------------------------------------------------------- */

esp_err_t ct_prov_ble_start(const ct_prov_ble_identity_t *identity,
                            ct_prov_ble_request_cb_t cb, void *cb_arg)
{
    if (s_started) {
        return ESP_OK;
    }
    if (identity == NULL || !ct_prov_name_valid(identity->device_name) ||
        identity->model_code == 0x00 || identity->model_code == 0xFF) {
        return ESP_ERR_INVALID_ARG;
    }

    s_identity = *identity;
    s_req_cb = cb;
    s_req_cb_arg = cb_arg;
    s_flags = CT_PROV_FLAG_PROVISIONABLE;
    s_stopping = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    session_reset();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SMP pairing failures log only at DEBUG and the default runtime level
     * is INFO, so without this the pairing trace stays invisible.  Requires
     * CONFIG_LOG_MAXIMUM_LEVEL=4 in sdkconfig to compile the calls in. */
    esp_log_level_set("NimBLE", ESP_LOG_DEBUG);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Doc 4.4: encrypted link for credentials, Just Works as the floor, LE
     * Secure Connections preferred.  Bonding with LTK distribution is
     * deliberate: OS-level pairing APIs (Windows PairAsync, Android
     * createBond) report failure when the peripheral offers no key to
     * store, so a no-bonding posture breaks every system pairing path.
     * SC stays off for now: with it on, pairing against a Realtek USB
     * dongle (Windows 10) died mid-procedure and took the link with it;
     * legacy Just Works meets the doc's floor.  The Security Manager
     * itself is compiled in via CONFIG_BT_NIMBLE_SECURITY_ENABLE
     * (Kconfig-only in this NimBLE; ble_hs_cfg has no sm enable member). */
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    /* Bonding is only useful when the host has key-store callbacks.  Without
     * these, the SMP exchange can advertise bonding but cannot retain the LTK,
     * which Windows reports as a failed system pairing. */
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(s_identity.device_name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service registration failed: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = watchdog_cb,
        .name = "ct_prov_wdt",
    };
    err = esp_timer_create(&timer_args, &s_watchdog);
    if (err == ESP_OK) {
        esp_timer_start_periodic(s_watchdog, CT_PROV_WATCHDOG_PERIOD_US);
    } else {
        ESP_LOGW(TAG, "reassembly watchdog unavailable: %s",
                 esp_err_to_name(err));
    }

    s_started = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "CT-Prov server up as %s", s_identity.device_name);
    return ESP_OK;
}

void ct_prov_ble_stop(void)
{
    if (!s_started) {
        return;
    }
    s_stopping = true;
    if (s_watchdog != NULL) {
        esp_timer_stop(s_watchdog);
        esp_timer_delete(s_watchdog);
        s_watchdog = NULL;
    }
    const int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: rc=%d", rc);
        return;
    }
    nimble_port_deinit();
    s_started = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "CT-Prov server stopped");
}

esp_err_t ct_prov_ble_set_flags(uint8_t flags)
{
    const uint8_t known = CT_PROV_FLAG_BUSY | CT_PROV_FLAG_PROVISIONABLE;
    if ((flags & ~known) != 0 || (flags & known) == known) {
        return ESP_ERR_INVALID_ARG;
    }
    s_flags = flags;
    if (s_started && !s_stopping &&
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        /* Idle: refresh the on-air payload right away. */
        ble_gap_adv_stop();
        advertise();
    }
    return ESP_OK;
}

bool ct_prov_ble_client_connected(void)
{
    /* Called from the config-mode idle timer task, while s_conn_handle is
     * written by the NimBLE host task. A single aligned uint16_t read is
     * atomic here, the same unlocked read watchdog_cb already relies on. */
    return s_started && s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
