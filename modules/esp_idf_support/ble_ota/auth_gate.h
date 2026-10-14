#ifndef SHIZUKU_ESP_BLE_OTA_AUTH_GATE_H
#define SHIZUKU_ESP_BLE_OTA_AUTH_GATE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NimBLE BLE_GATT_ACCESS_OP_WRITE_CHR is 1. This testable function is called
// directly by the production GATT callback before payload extraction/enqueue.
int shizuku_esp_ota_gatt_access_status(uint8_t operation);

#ifdef __cplusplus
}
#endif
#endif
