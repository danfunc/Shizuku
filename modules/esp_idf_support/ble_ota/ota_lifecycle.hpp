#ifndef SHIZUKU_ESP_BLE_OTA_LIFECYCLE_HPP
#define SHIZUKU_ESP_BLE_OTA_LIFECYCLE_HPP

namespace shizuku::esp_idf_support::ble_ota {
enum class phase { idle, writing, validated, activated, failed };
constexpr phase after_begin(bool ok) { return ok ? phase::writing : phase::idle; }
constexpr phase after_validate(phase current, bool ok) {
  return current == phase::writing ? (ok ? phase::validated : phase::failed) : phase::failed;
}
constexpr bool may_activate(phase current) { return current == phase::validated; }
constexpr phase after_abort() { return phase::idle; }
}
#endif
