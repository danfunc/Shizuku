#ifndef SHIZUKU_ESP_BLE_OTA_SERVICE_H
#define SHIZUKU_ESP_BLE_OTA_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

struct ble_gap_event;

// Call during NimBLE GATT initialization before advertising. The existing
// Shizuku GATT integration is intentionally a separate patch proposal.
int shizuku_esp_ota_register_service(void);

// Forward every event from the existing BLE GAP callback to this helper. XNOC
// waits for the configured notification TX-complete event or indication ACK.
int shizuku_esp_ota_gap_event(struct ble_gap_event *event);

// Call from a confirmed post-boot health/selftest path to accept the candidate.
int shizuku_esp_ota_mark_running_app_valid(void);

#ifdef __cplusplus
}
#endif
#endif
