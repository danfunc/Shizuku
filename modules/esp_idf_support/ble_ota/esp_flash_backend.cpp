#include "esp_flash_backend.hpp"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include <cstdint>
#include <cstring>

namespace shizuku::esp_idf_support::ble_ota {
namespace {
constexpr uint32_t kOtaSlotBytes = 0x1F0000u;
constexpr uint32_t kSectorBytes = 4096u;

bool begin(void *opaque, uint32_t total_bytes, uint32_t, xno::ota::protocol::Mode) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s || s->handle_open || total_bytes == 0 || total_bytes > kOtaSlotBytes)
    return false;
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || next->type != ESP_PARTITION_TYPE_APP ||
      (next->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
       next->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
      next->size < total_bytes || next->size > kOtaSlotBytes)
    return false;
  esp_ota_handle_t handle = 0;
  if (esp_ota_begin(next, total_bytes, &handle) != ESP_OK)
    return false;
  s->partition = next;
  s->handle = handle;
  s->image_bytes = total_bytes;
  s->handle_open = true;
  s->image_validated = false;
  s->commit_acknowledged = false;
  s->lifecycle = after_begin(true);
  return true;
}

bool write(void *opaque, uint32_t offset, const uint8_t *data, uint32_t length) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s || !s->handle_open || !s->partition || !data || length == 0 ||
      offset > s->image_bytes || length > s->image_bytes - offset)
    return false;
  return esp_ota_write_with_offset(s->handle, data, length, offset) == ESP_OK;
}

bool read(void *opaque, uint32_t offset, uint8_t *data, uint32_t length) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s || !s->partition || !data || length == 0 ||
      offset > s->image_bytes || length > s->image_bytes - offset)
    return false;
  return esp_partition_read(s->partition, offset, data, length) == ESP_OK;
}

bool validate(void *opaque, uint32_t total_bytes) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s || !s->handle_open || !s->partition || total_bytes != s->image_bytes)
    return false;
  const esp_err_t rc = esp_ota_end(s->handle);
  s->handle_open = false; // esp_ota_end consumes the handle on both outcomes.
  if (rc != ESP_OK) {
    s->lifecycle = after_validate(s->lifecycle, false);
    return false;
  }
  s->image_validated = true;
  s->lifecycle = after_validate(s->lifecycle, true);
  return true;
}

bool activate(void *opaque) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s || !s->partition || !s->image_validated || !s->commit_acknowledged ||
      !may_activate(s->lifecycle))
    return false;
  if (esp_ota_set_boot_partition(s->partition) != ESP_OK)
    return false;
  s->lifecycle = phase::activated;
  return true;
}

void abort(void *opaque) {
  auto *s = static_cast<flash_session *>(opaque);
  if (!s) return;
  if (s->handle_open) {
    (void)esp_ota_abort(s->handle);
    s->handle_open = false;
  }
  s->partition = nullptr;
  s->image_bytes = 0;
  s->image_validated = false;
  s->commit_acknowledged = false;
  s->lifecycle = after_abort();
}

} // namespace

xno::ota::protocol::Callbacks callbacks(flash_session *session,
                                         void (*event)(void *, const xno::ota::protocol::Event &)) {
  return {session, begin, write, read, validate, activate, abort, event};
}

flash_session &global_session() {
  static flash_session session{};
  return session;
}

} // namespace shizuku::esp_idf_support::ble_ota

extern "C" esp_err_t shizuku_esp_ota_mark_running_app_valid(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) return ESP_ERR_NOT_FOUND;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t state_rc = esp_ota_get_state_partition(running, &state);
  if (state_rc != ESP_OK) return state_rc;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
  return esp_ota_mark_app_valid_cancel_rollback();
}
