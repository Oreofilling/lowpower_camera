#pragma once

#include "esp_err.h"

/**
 * @brief Start the teaching-only BLE provisioning demo.
 *
 * The demo exposes a Nordic-UART-style GATT service.  It is intentionally
 * plaintext and only accepts the harmless commands documented in
 * docs/BLE_PROV_DEMO.md; it does not read or write Wi-Fi credentials.
 */
esp_err_t ble_prov_demo_start(void);

/**
 * @brief Stop advertising, disconnect peers, and release the NimBLE stack.
 */
void ble_prov_demo_stop(void);
