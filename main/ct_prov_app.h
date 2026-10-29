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

#ifdef __cplusplus
}
#endif

#endif /* __CT_PROV_APP_H__ */
