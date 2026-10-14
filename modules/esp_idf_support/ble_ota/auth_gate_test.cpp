#include "auth_gate.h"
#include <cassert>

int main() {
  constexpr uint8_t kNimbleWriteCharacteristic = 1u;
  constexpr uint8_t kNimbleReadCharacteristic = 0u;
#if defined(SHIZUKU_ESP_OTA_TEST_WRITES) && SHIZUKU_ESP_OTA_TEST_WRITES == 1
  assert(shizuku_esp_ota_gatt_access_status(kNimbleWriteCharacteristic) == 0);
#else
  assert(shizuku_esp_ota_gatt_access_status(kNimbleWriteCharacteristic) == 0x08);
#endif
  assert(shizuku_esp_ota_gatt_access_status(kNimbleReadCharacteristic) == 0);
  return 0;
}
