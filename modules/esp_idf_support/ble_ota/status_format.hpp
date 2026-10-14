#ifndef SHIZUKU_ESP_BLE_OTA_STATUS_FORMAT_HPP
#define SHIZUKU_ESP_BLE_OTA_STATUS_FORMAT_HPP
#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
namespace shizuku::esp_idf_support::ble_ota {
enum class status_delivery { none, notification, indication };
status_delivery format_status(const xno::ota::protocol::Event &event,
                              char *out, size_t capacity, size_t *length);
}
#endif
