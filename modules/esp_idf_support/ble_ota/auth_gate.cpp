#include "auth_gate.h"

namespace {
constexpr uint8_t kWriteCharacteristicOperation = 1u;
constexpr int kAttInsufficientAuthorization = 0x08;
constexpr bool ota_authorized() { return false; }
}

extern "C" int shizuku_esp_ota_gatt_access_status(uint8_t operation) {
  if (operation != kWriteCharacteristicOperation) return 0;
  return ota_authorized() ? 0 : kAttInsufficientAuthorization;
}
