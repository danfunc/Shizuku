#include "esp_ble_service.h"
#include "auth_gate.h"
#include "esp_flash_backend.hpp"
#include "status_format.hpp"
#include "protocol.hpp"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include <cstdint>
#include <cstring>

namespace {
using namespace shizuku::esp_idf_support::ble_ota;
using namespace xno::ota::protocol;
using xno::ota::format_status;
using xno::ota::phase;
using xno::ota::status_delivery;
constexpr uint32_t kMaxImageBytes = 0x1F0000u;
constexpr uint16_t kRxMax = 512u;
constexpr UBaseType_t kRxQueueDepth = 2u;
constexpr UBaseType_t kTxQueueDepth = 2u;
constexpr TickType_t kIndicationTimeout = pdMS_TO_TICKS(5000u);

struct rx_packet {
  uint16_t conn_handle;
  uint16_t length;
  uint8_t reset;
  uint8_t bytes[kRxMax];
};
static_assert(sizeof(rx_packet) == 518);
static_assert(kRxQueueDepth * sizeof(rx_packet) == 1036);
struct tx_completion {
  uint16_t conn_handle;
  uint16_t attr_handle;
  int status;
  uint8_t indication;
};

static ble_uuid128_t g_nus_service_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static ble_uuid128_t g_nus_rx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static ble_uuid128_t g_nus_tx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);
static ble_uuid128_t g_ota_service_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x20,0x40,0x6e);
static ble_uuid128_t g_ota_rx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x20,0x40,0x6e);

static uint16_t g_tx_handle = 0;
static QueueHandle_t g_rx_queue = nullptr;
static QueueHandle_t g_tx_completion_queue = nullptr;
static TaskHandle_t g_worker_task = nullptr;
static volatile uint16_t g_owner_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool g_tx_notifications_enabled = false;
static volatile bool g_tx_indications_enabled = false;
static volatile bool g_commit_waiting = false;
static volatile bool g_commit_expected_indication = false;
static bool g_registration_attempted = false;
static bool g_registered = false;
static uint16_t g_processing_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t g_encoded[kMaxEncodedBytes]{};
static uint8_t g_decoded[4096]{};
static uint32_t g_received_bitmap[(kMaxImageBytes / 4096u + 31u) / 32u]{};
static tiny_inflate::state g_inflate_state{};

Workspace make_workspace() {
  return {g_encoded, sizeof(g_encoded), g_decoded, sizeof(g_decoded),
          g_received_bitmap,
          sizeof(g_received_bitmap) / sizeof(g_received_bitmap[0]),
          &g_inflate_state};
}

bool is_session_owner(uint16_t conn_handle) {
  return conn_handle != BLE_HS_CONN_HANDLE_NONE &&
         g_owner_conn_handle == conn_handle;
}

bool notify_line(uint16_t conn_handle, const char *text, size_t length,
                 bool indication) {
  if (!is_session_owner(conn_handle) || g_tx_handle == 0 || !text ||
      length == 0 || length > UINT16_MAX)
    return false;
  struct os_mbuf *om = ble_hs_mbuf_from_flat(text, static_cast<uint16_t>(length));
  if (!om) return false;
  const int rc = indication
      ? ble_gatts_indicate_custom(conn_handle, g_tx_handle, om)
      : ble_gatts_notify_custom(conn_handle, g_tx_handle, om);
  return rc == 0;
}

bool wait_for_commit_tx(uint16_t conn_handle, uint16_t attr_handle,
                        bool indication) {
  if (!g_tx_completion_queue) return false;
  tx_completion done{};
  const TickType_t start_timeout = kIndicationTimeout;
  const TickType_t start = xTaskGetTickCount();
  while ((xTaskGetTickCount() - start) < start_timeout) {
    const TickType_t left = start_timeout - (xTaskGetTickCount() - start);
    if (xQueueReceive(g_tx_completion_queue, &done, left) != pdTRUE) return false;
    if (done.conn_handle == conn_handle && done.attr_handle == attr_handle &&
        indication && done.indication && done.status == BLE_HS_EDONE)
      return true;
    if (done.conn_handle == conn_handle && done.attr_handle == attr_handle &&
        !indication && !done.indication && done.status == 0)
      return true;
    if (done.conn_handle == conn_handle && done.indication == indication &&
        done.status != 0 && done.status != BLE_HS_EDONE)
      return false;
  }
  return false;
}

int gatt_access(uint16_t conn_handle, uint16_t, struct ble_gatt_access_ctxt *ctxt,
                void *) {
  if (!ctxt) return BLE_ATT_ERR_UNLIKELY;
  // Denies every characteristic write before reading or enqueueing payload bytes.
  // The gate lives in XNO; only the OTA-only test build define can open it.
  const int gate_status = xno_ota_gatt_access_status(ctxt->op);
  if (gate_status != 0) return gate_status;
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (!is_session_owner(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!g_rx_queue) return BLE_ATT_ERR_INSUFFICIENT_RES;
    rx_packet packet{};
    packet.conn_handle = conn_handle;
    uint16_t copied = 0;
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, packet.bytes,
                                      sizeof(packet.bytes), &copied);
    if (rc != 0 || copied == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    packet.length = copied;
    return xQueueSend(g_rx_queue, &packet, 0) == pdTRUE
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC && ctxt->chr &&
      ctxt->chr->uuid == &g_nus_tx_uuid.u) {
    return is_session_owner(conn_handle) ? 0 : BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  }
  return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static ble_gatt_chr_def g_nus_chars[3]{};
static ble_gatt_chr_def g_ota_chars[2]{};
static ble_gatt_svc_def g_services[3]{};

void init_gatt_definitions() {
  g_nus_chars[0].uuid = &g_nus_rx_uuid.u;
  g_nus_chars[0].access_cb = gatt_access;
  g_nus_chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
  g_nus_chars[1].uuid = &g_nus_tx_uuid.u;
  g_nus_chars[1].access_cb = gatt_access;
  g_nus_chars[1].flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE;
  g_nus_chars[1].val_handle = &g_tx_handle;
  g_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_services[0].uuid = &g_nus_service_uuid.u;
  g_services[0].characteristics = g_nus_chars;

  g_ota_chars[0].uuid = &g_ota_rx_uuid.u;
  g_ota_chars[0].access_cb = gatt_access;
  g_ota_chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
  g_services[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_services[1].uuid = &g_ota_service_uuid.u;
  g_services[1].characteristics = g_ota_chars;
}

void protocol_event(void *, const Event &event) {
  char line[96]{};
  size_t length = 0;
  const status_delivery delivery = format_status(event, line, sizeof(line), &length);
  auto &session = global_session();
  if (delivery == status_delivery::none) {
    if (event.type == EventType::CommitReady) session.commit_acknowledged = false;
    return;
  }
  if (event.type == EventType::CommitReady) {
    const bool use_indication = !g_tx_notifications_enabled && g_tx_indications_enabled;
    if (!g_tx_notifications_enabled && !g_tx_indications_enabled) {
      session.commit_acknowledged = false;
      return;
    }
    tx_completion stale{};
    while (xQueueReceive(g_tx_completion_queue, &stale, 0) == pdTRUE) {}
    g_commit_expected_indication = use_indication;
    g_commit_waiting = true;
    const bool sent = notify_line(g_processing_conn_handle, line, length, use_indication);
    session.commit_acknowledged = sent &&
        wait_for_commit_tx(g_processing_conn_handle, g_tx_handle, use_indication);
    g_commit_waiting = false;
    return;
  }
  (void)notify_line(g_processing_conn_handle, line, length, false);
}

void worker(void *) {
  Workspace workspace = make_workspace();
  auto &session = global_session();
  Protocol protocol(workspace, callbacks(&session, protocol_event), kMaxImageBytes);
  if (!protocol.valid()) {
    g_worker_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  rx_packet packet{};
  for (;;) {
    if (xQueueReceive(g_rx_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
    if (packet.reset) {
      protocol.abort();
      continue;
    }
    if (!is_session_owner(packet.conn_handle) || packet.length == 0 ||
        packet.length > sizeof(packet.bytes))
      continue;
    g_processing_conn_handle = packet.conn_handle;
    (void)protocol.feed(packet.bytes, packet.length);
    if (session.lifecycle == phase::activated) {
      // CommitReady was delivered before the backend selected this OTA slot;
      // reboot only after activate returned. Indications are ACKed when the
      // central subscribes to them; notifications wait for NimBLE TX completion.
      esp_restart();
    }
  }
}

void cleanup_registration(bool allow_retry) {
  if (g_worker_task) {
    vTaskDelete(g_worker_task);
    g_worker_task = nullptr;
  }
  if (g_tx_completion_queue) {
    vQueueDelete(g_tx_completion_queue);
    g_tx_completion_queue = nullptr;
  }
  if (g_rx_queue) {
    vQueueDelete(g_rx_queue);
    g_rx_queue = nullptr;
  }
  g_registration_attempted = !allow_retry;
}
} // namespace

extern "C" int shizuku_esp_ota_register_service(void) {
  if (g_registered) return 0;
  if (g_registration_attempted) return BLE_HS_EALREADY;
  g_registration_attempted = true;
  Protocol check(make_workspace(), callbacks(&global_session(), protocol_event),
                 kMaxImageBytes);
  if (!check.valid()) {
    cleanup_registration(true);
    return BLE_HS_EINVAL;
  }
  g_rx_queue = xQueueCreate(kRxQueueDepth, sizeof(rx_packet));
  g_tx_completion_queue = xQueueCreate(kTxQueueDepth, sizeof(tx_completion));
  if (!g_rx_queue || !g_tx_completion_queue) {
    cleanup_registration(true);
    return BLE_HS_ENOMEM;
  }
  if (xTaskCreate(worker, "ota_worker", 4096, nullptr, 5, &g_worker_task) != pdPASS) {
    cleanup_registration(true);
    return BLE_HS_ENOMEM;
  }
  init_gatt_definitions();
  int rc = ble_gatts_count_cfg(g_services);
  if (rc != 0) {
    cleanup_registration(true);
    return rc;
  }
  rc = ble_gatts_add_svcs(g_services);
  if (rc != 0) {
    // add_svcs failure may have partially registered attributes; keep this
    // instance fail-closed and do not retry the GATT registration.
    cleanup_registration(false);
    return rc;
  }
  g_registered = true;
  return 0;
}

extern "C" int shizuku_esp_ota_gap_event(struct ble_gap_event *event) {
  if (!event) return BLE_HS_EINVAL;
  if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
    if (g_owner_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        g_owner_conn_handle != event->connect.conn_handle) {
      (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      return BLE_HS_EBUSY;
    }
    g_owner_conn_handle = event->connect.conn_handle;
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
    const uint16_t conn_handle = event->disconnect.conn.conn_handle;
    if (g_owner_conn_handle == conn_handle) {
      g_owner_conn_handle = BLE_HS_CONN_HANDLE_NONE;
      g_tx_notifications_enabled = false;
      g_tx_indications_enabled = false;
      if (g_tx_completion_queue && g_commit_waiting) {
        tx_completion failed{conn_handle, g_tx_handle, BLE_HS_ENOTCONN,
                             static_cast<uint8_t>(g_commit_expected_indication ? 1 : 0)};
        (void)xQueueSend(g_tx_completion_queue, &failed, 0);
      }
      if (g_rx_queue) {
        rx_packet discarded{};
        while (xQueueReceive(g_rx_queue, &discarded, 0) == pdTRUE) {}
        rx_packet reset{};
        reset.reset = 1;
        (void)xQueueSendToFront(g_rx_queue, &reset, 0);
      }
    }
  } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE &&
             event->subscribe.attr_handle == g_tx_handle &&
             event->subscribe.conn_handle == g_owner_conn_handle) {
    g_tx_notifications_enabled = event->subscribe.cur_notify != 0;
    g_tx_indications_enabled = event->subscribe.cur_indicate != 0;
  } else if (event->type == BLE_GAP_EVENT_NOTIFY_TX && g_commit_waiting &&
             event->notify_tx.attr_handle == g_tx_handle &&
             event->notify_tx.conn_handle == g_owner_conn_handle &&
             g_tx_completion_queue) {
    tx_completion done{event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                       event->notify_tx.status,
                       static_cast<uint8_t>(event->notify_tx.indication ? 1 : 0)};
    (void)xQueueSend(g_tx_completion_queue, &done, 0);
  }
  return 0;
}
