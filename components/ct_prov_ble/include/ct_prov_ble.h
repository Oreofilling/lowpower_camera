/*
 * CT-Prov/1 GATT server (protocol doc sections 3-6, 11, 13).
 * Owns the NimBLE service (C2D write / D2C indicate / META read), legacy
 * advertising, link security, and the fragment + envelope pipeline.
 * Request dispatch is delegated to an application callback; session.close
 * is handled by the server itself (doc 11).
 */
#ifndef CT_PROV_BLE_H
#define CT_PROV_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ct_prov_envelope.h"

/* device_name: 9..17 chars + NUL (doc 4.2.1). */
#define CT_PROV_BLE_NAME_MAX 18

/* Server identity derived from factory info (doc 4.2.2). */
typedef struct {
    uint8_t model_code;    /* 0x01 NE101, 0x02 NE301 */
    uint8_t mac_suffix[3]; /* canonical MAC last 3 bytes, display order */
    char device_name[CT_PROV_BLE_NAME_MAX];
} ct_prov_ble_identity_t;

/*
 * Request handler: fill resp_json with one complete response envelope and
 * return its length. Return 0 to make the server answer INTERNAL_ERROR.
 * Runs in the NimBLE host task and must not block (doc 13).
 */
typedef size_t (*ct_prov_ble_request_cb_t)(void *arg,
                                           const ct_prov_envelope_t *req,
                                           char *resp_json, size_t cap);

/*
 * Bring up the stack, register the GATT service and start advertising.
 * Fails if the identity violates the doc 4.2 name/model rules, so callers
 * can fall back to their own configuration surface.
 */
esp_err_t ct_prov_ble_start(const ct_prov_ble_identity_t *identity,
                            ct_prov_ble_request_cb_t cb, void *cb_arg);

/* Tear down the stack and release the connection. Safe to call twice. */
void ct_prov_ble_stop(void);

/*
 * Update BUSY/PROVISIONABLE in advertising data and META (doc 4.2.2).
 * Restarts advertising when idle; takes effect on the next connection
 * while a session is active.
 */
esp_err_t ct_prov_ble_set_flags(uint8_t flags);

/*
 * True while a CT-Prov client holds a GATT connection. Config mode polls
 * this from its idle timer so an active provisioning session counts as
 * user presence and does not trip the no-operation sleep timeout; once
 * the client disconnects, the caller's idle count restarts from zero.
 */
bool ct_prov_ble_client_connected(void);

#endif /* CT_PROV_BLE_H */
