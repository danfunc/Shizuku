#include "auth_gate.h"

namespace {
constexpr uint8_t kWriteCharacteristicOperation = 1u;
constexpr int kAttInsufficientAuthorization = 0x08;
#if defined(SHIZUKU_ESP_OTA_TEST_WRITES) && SHIZUKU_ESP_OTA_TEST_WRITES == 1
constexpr bool ota_authorized() { return true; }
#else
constexpr bool ota_authorized() { return false; }
#endif
}

extern "C" int shizuku_esp_ota_gatt_access_status(uint8_t operation) {
  if (operation != kWriteCharacteristicOperation) return 0;
  return ota_authorized() ? 0 : kAttInsufficientAuthorization;
}
