#ifndef SHIZUKU_ESP_FLASH_BACKEND_HPP
#define SHIZUKU_ESP_FLASH_BACKEND_HPP

#include "protocol.hpp"
#include "ota_lifecycle.hpp"
#include "esp_partition.h"
#include "esp_ota_ops.h"

namespace shizuku::esp_idf_support::ble_ota {
struct flash_session {
  const esp_partition_t *partition = nullptr;
  esp_ota_handle_t handle = 0;
  uint32_t image_bytes = 0;
  bool handle_open = false;
  bool image_validated = false;
  bool commit_acknowledged = false;
  xno::ota::phase lifecycle = xno::ota::phase::idle;
};

flash_session &global_session();
xno::ota::protocol::Callbacks callbacks(
    flash_session *session,
    void (*event)(void *, const xno::ota::protocol::Event &));
}
#endif
