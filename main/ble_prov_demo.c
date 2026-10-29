#include "ble_prov_demo.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "config.h"
#include "utils.h"

#define BLE_DEMO_COMMAND_MAX_LEN 64
#define BLE_DEMO_RESPONSE_MAX_LEN 20

/*
 * Nordic UART Service UUIDs.  NimBLE stores 128-bit UUIDs least-significant
 * byte first, so the byte arrays below look reversed from their text form.
 *
 * Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E (phone writes)
 * TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E (device notifies)
 */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static const char *TAG = "BLE_PROV_DEMO";

static uint8_t s_addr_type;
static uint16_t s_tx_value_handle;
static char s_device_name[24];
static char s_info_response[BLE_DEMO_RESPONSE_MAX_LEN];
static uint8_t s_response[BLE_DEMO_RESPONSE_MAX_LEN];
static uint16_t s_response_len;
static bool s_started;
static bool s_stopping;

static int ble_demo_gap_event(struct ble_gap_event *event, void *arg);

static void ble_demo_set_response(const char *response)
{
    s_response_len = strnlen(response, sizeof(s_response));
    memcpy(s_response, response, s_response_len);
}

static int ble_demo_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, s_response, s_response_len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_demo_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t command[BLE_DEMO_COMMAND_MAX_LEN + 1];
    uint16_t command_len = OS_MBUF_PKTLEN(ctxt->om);
    uint16_t copied_len = 0;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (command_len == 0 || command_len > BLE_DEMO_COMMAND_MAX_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    int rc = ble_hs_mbuf_to_flat(ctxt->om, command,
                                 BLE_DEMO_COMMAND_MAX_LEN, &copied_len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    while (copied_len > 0 &&
           (command[copied_len - 1] == '\r' || command[copied_len - 1] == '\n')) {
        copied_len--;
    }
    command[copied_len] = '\0';

    ESP_LOGI(TAG, "RX write: '%s'", (char *)command);
    if (strcmp((char *)command, "ping") == 0) {
        ble_demo_set_response("pong");
    } else if (strcmp((char *)command, "info") == 0) {
        ble_demo_set_response(s_info_response);
    } else {
        ble_demo_set_response("ERR:use ping/info");
    }

    /*
     * This schedules a notification for every client subscribed to TX.  If
     * notifications were not enabled in the phone app, TX can still be read.
     */
    ble_gatts_chr_updated(s_tx_value_handle);
    ESP_LOGI(TAG, "TX response: '%.*s'", s_response_len, s_response);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = ble_demo_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = ble_demo_tx_access,
                .val_handle = &s_tx_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static int ble_demo_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    return rc;
}

static void ble_demo_advertise(void)
{
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields scan_response = {0};
    struct ble_gap_adv_params adv_params = {0};

    if (s_stopping) {
        return;
    }

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = (uint8_t *)s_device_name;
    adv_fields.name_len = strlen(s_device_name);
    adv_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set advertising data failed: rc=%d", rc);
        return;
    }

    scan_response.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    scan_response.num_uuids128 = 1;
    scan_response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(TAG, "set scan response failed: rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_demo_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start advertising failed: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising as %s", s_device_name);
}

static int ble_demo_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->link_estab.status == 0) {
            ESP_LOGI(TAG, "phone connected, handle=%u", event->link_estab.conn_handle);
        } else {
            ESP_LOGW(TAG, "connection failed, status=%d", event->link_estab.status);
            ble_demo_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected, reason=%d", event->disconnect.reason);
        ble_demo_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising completed, reason=%d", event->adv_complete.reason);
        ble_demo_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_value_handle) {
            ESP_LOGI(TAG, "TX notifications %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void ble_demo_on_sync(void)
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

    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }
    ble_demo_advertise();
}

static void ble_demo_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void ble_demo_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_demo_prepare_identity(void)
{
    deviceInfo_t device = {0};
    uint8_t mac[6] = {0};

    cfg_get_device_info(&device);
    if (is_valid_mac(device.mac)) {
        mac_str2hex(device.mac, mac);
    }

    const char *model = device.model[0] != '\0' ? device.model : "NE101";
    snprintf(s_device_name, sizeof(s_device_name), "%.10s-BLE-%02X%02X",
             model, mac[4], mac[5]);
    snprintf(s_info_response, sizeof(s_info_response), "model=%.13s", model);
}

esp_err_t ble_prov_demo_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ble_demo_prepare_identity();
    ble_demo_set_response("ready");
    s_stopping = false;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_demo_on_reset;
    ble_hs_cfg.sync_cb = ble_demo_on_sync;

    int rc = ble_demo_gatt_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service init failed: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_started = true;
    nimble_port_freertos_init(ble_demo_host_task);
    ESP_LOGW(TAG, "teaching demo only: GATT traffic is plaintext; do not send passwords");
    return ESP_OK;
}

void ble_prov_demo_stop(void)
{
    if (!s_started) {
        return;
    }

    s_stopping = true;
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: rc=%d", rc);
        return;
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return;
    }

    s_started = false;
    ESP_LOGI(TAG, "BLE demo stopped");
}
