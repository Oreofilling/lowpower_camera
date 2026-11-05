#ifndef __CT_PROV_APP_H__
#define __CT_PROV_APP_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the CT-Prov/1 BLE provisioning server (config mode)
 *
 * Derives the BLE identity (model code, MAC suffix, device name) from
 * factory config and serves device.info.get over the doc 4 GATT service.
 * Returns an error when the factory info does not map onto the doc 4.2
 * identity rules, so the caller can keep its other config surfaces.
 */
esp_err_t ct_prov_app_start(void);

/**
 * @brief Stop the CT-Prov/1 BLE provisioning server
 */
void ct_prov_app_stop(void);

/**
 * @brief Dispatch one CT-Prov request envelope over the softAP HTTP
 *        binding (POST /api/v1/ctprov)
 *
 * body holds one complete request envelope JSON document. The response
 * envelope is written to resp and its length returned; 0 means the body
 * was not a usable request (the caller answers HTTP 400). Serialized
 * against the BLE request path because both share the dispatch scratch
 * buffer; session.close succeeds like any op and has no disconnect
 * side effect over HTTP.
 */
size_t ct_prov_app_handle_http(const char *body, size_t len, char *resp,
                               size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* __CT_PROV_APP_H__ */
