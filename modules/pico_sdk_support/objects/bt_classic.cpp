// BT core の Classic HID host サービス (汎用)。機種固有の初期化は外 (車) が行う。
// ★ここの関数はすべて owner スレッドからだけ呼ぶ (btstack は非スレッドセーフ)。
//   外とは hid_rx / hid_tx の STREAM だけで話す (設計: bt-core-port/DESIGN.md)。
#include "shizuku/objects/bt_classic.hpp"

extern "C" {
#include "btstack.h"
#include "classic/hid_host.h"
#include "classic/sdp_server.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
}
#include "shizuku/kernel.hpp"
#include <cstdio>
#include <cstring>

namespace shizuku {
namespace objects {
namespace bt_classic {
namespace {

using BOARD = shizuku::KERNEL::BOARD;

void (*g_emit)(const hid_frame &) = nullptr;

btstack_packet_callback_registration_t g_hci_reg;
uint8_t g_descriptor_storage[256];

bd_addr_t g_target;
bool g_target_set = false;
bool g_hci_working = false;
bool g_open = false;
bool g_connecting = false;
uint16_t g_cid = 0;

// 登録し直し: inquiry 中は自動再接続・incoming を止める (inquiry と page は同時に走らせない)。
bool g_scanning = false;
bool g_scan_stopping = false; // gap_inquiry_stop 済みで COMPLETE 待ち (= 理由は「止めた」)
// HCI が WORKING になる前に来た TX_LIST_BONDS は、WORKING のイベントで返す
// (link key DB は WORKING 以降でないと使えない BTstack の port がある)。
bool g_list_pending = false;
// link key は NVM_NUM_LINK_KEYS (config_classic) 件までしか持たないので上限もそれに合わせる。
constexpr uint8_t kMaxBondsReported = 8;
// HCI Inquiry_Length の範囲 (1.28 秒単位)。BTstack は hci.c 内の private define しか持たない。
constexpr uint32_t kInquiryUnitsMin = 0x01;
constexpr uint32_t kInquiryUnitsMax = 0x30;

// 再試行は btstack の run loop タイマー (イベント起点)。接続処理中は connect を
// 呼ばない (既存 connection があると COMMAND_DISALLOWED, hid_host.c)。失敗が続くと
// outgoing の page がページスキャンを塞いで incoming を取りこぼすので 3 秒→10 秒へ伸ばす。
constexpr uint32_t kRetryMinMs = 3000;
constexpr uint32_t kRetryMaxMs = 10000;
constexpr uint32_t kConnectingGuardMs = 15000;
uint32_t g_retry_ms = kRetryMinMs;
btstack_timer_source_t g_timer;
bool g_timer_armed = false;

void emit(uint8_t kind, const uint8_t *data, uint8_t len) {
  if (g_emit == nullptr)
    return;
  hid_frame f;
  memset(&f, 0, sizeof(f));
  f.kind = kind;
  if (len > sizeof(f.data))
    len = sizeof(f.data);
  f.len = len;
  if (len != 0)
    memcpy(f.data, data, len);
  g_emit(f);
}

bool is_target(const uint8_t *addr) {
  return g_target_set && memcmp(addr, g_target, 6) == 0;
}

void arm_timer(uint32_t ms) {
  if (g_timer_armed)
    btstack_run_loop_remove_timer(&g_timer);
  btstack_run_loop_set_timer(&g_timer, ms);
  btstack_run_loop_add_timer(&g_timer);
  g_timer_armed = true;
}

void disarm_timer() {
  if (g_timer_armed)
    btstack_run_loop_remove_timer(&g_timer);
  g_timer_armed = false;
}

void connect_now();

void schedule_retry() {
  g_connecting = false;
  // target 未設定 (TX_FORGET 後) やスキャン中は再試行しない。スキャンが終わるか
  // TX_CONNECT が来たときに connect_now() が再開する。
  if (!g_target_set || g_scanning)
    return;
  BOARD::diag_printf("[BT_CLASSIC] next connect in %lu ms\n",
                     (unsigned long)g_retry_ms);
  arm_timer(g_retry_ms);
  g_retry_ms += g_retry_ms / 2;
  if (g_retry_ms > kRetryMaxMs)
    g_retry_ms = kRetryMaxMs;
}

// connecting のまま固まった接続の掃除。ACL がまだ確立していない (handle 無効) 間は
// gap_disconnect が COMMAND_DISALLOWED になるので呼ばない。
void clear_stale(const char *why) {
  BOARD::diag_printf("[BT_CLASSIC] clearing stale connection (%s) cid=%04x\n", why,
                     g_cid);
  if (g_cid != 0)
    hid_host_disconnect(g_cid);
  if (!g_target_set)
    return;
  hci_connection_t *acl =
      hci_connection_for_bd_addr_and_type(g_target, BD_ADDR_TYPE_ACL);
  if (acl != nullptr && acl->con_handle != HCI_CON_HANDLE_INVALID)
    gap_disconnect(acl->con_handle);
}

void timer_handler(btstack_timer_source_t *) {
  g_timer_armed = false;
  if (g_connecting) { // 15 秒たっても終わらない = 固まっている
    BOARD::diag_printf("[BT_CLASSIC] connect attempt timed out\n");
    clear_stale("connect timeout");
    g_connecting = false;
  }
  connect_now();
}

void connect_now() {
  if (!g_hci_working || !g_target_set || g_open || g_connecting || g_scanning)
    return;
  const uint8_t rc =
      hid_host_connect(g_target, HID_PROTOCOL_MODE_REPORT, &g_cid);
  BOARD::diag_printf("[BT_CLASSIC] hid_host_connect rc=0x%02x\n", rc);
  if (rc == ERROR_CODE_SUCCESS) {
    g_connecting = true;
    arm_timer(kConnectingGuardMs); // 完了しなければ timer_handler が掃除する
  } else {
    if (rc == ERROR_CODE_COMMAND_DISALLOWED)
      clear_stale("connect disallowed");
    schedule_retry();
  }
}

// ---- 登録し直し (bond 管理 / スキャン) ------------------------------------------
// 状態遷移:
//   通常 --TX_SCAN_START--> SCAN (接続中なら切断。タイマー解除。再試行・incoming 拒否)
//   SCAN --RX inquiry 時間切れ--> 通常 (target があれば connect_now) + RX_SCAN_DONE(0)
//   SCAN --TX_SCAN_STOP--> 通常 (同上) + RX_SCAN_DONE(1)
//   SCAN --TX_CONNECT--> target 更新 → inquiry 停止 → COMPLETE で通常へ戻り新 target に接続
//   通常/SCAN --TX_FORGET(全)--> 切断 + 鍵全消去 + target 解除 (スキャン自体は止めない)
void emit_scan_done(uint8_t reason, uint8_t status) {
  const uint8_t d[2] = {reason, status};
  emit(RX_SCAN_DONE, d, reason == 2 ? 2 : 1);
}

// スキャンの終わり (COMPLETE イベント / 開始失敗 / 停止) の共通処理。再入しても 1 回だけ。
void finish_scan(uint8_t reason, uint8_t status) {
  if (!g_scanning)
    return;
  g_scanning = false;
  g_scan_stopping = false;
  BOARD::diag_printf("[BT_CLASSIC] scan done reason=%u\n", (unsigned)reason);
  emit_scan_done(reason, status);
  if (g_target_set) {
    g_retry_ms = kRetryMinMs;
    connect_now();
  }
}

// 接続中・接続試行中の相手を切り、再試行の予約を解く (target は呼び出し側が決める)。
void drop_connection(const char *why) {
  if (g_open || g_connecting)
    clear_stale(why);
  g_connecting = false;
  disarm_timer();
}

void scan_start(uint8_t seconds) {
  if (g_scanning)
    return; // 二重開始は無視 (進行中の inquiry をそのまま使う)
  if (!g_hci_working) {
    emit_scan_done(2, ERROR_CODE_COMMAND_DISALLOWED);
    return;
  }
  // 1.28 秒単位に切り上げ。BTstack の範囲 (1..48 = 最大 61.44 秒) に丸める。
  uint32_t units = ((uint32_t)seconds * 100u + 127u) / 128u;
  if (units < kInquiryUnitsMin)
    units = kInquiryUnitsMin;
  if (units > kInquiryUnitsMax)
    units = kInquiryUnitsMax;
  // inquiry と接続 (page) は同時に走らせない。接続は先に切る。
  drop_connection("scan start");
  const int rc = gap_inquiry_start((uint8_t)units);
  BOARD::diag_printf("[BT_CLASSIC] inquiry start %us (units=%u) rc=0x%02x\n",
                     (unsigned)seconds, (unsigned)units, (unsigned)rc);
  if (rc != 0) {
    emit_scan_done(2, (uint8_t)rc);
    if (g_target_set)
      connect_now();
    return;
  }
  g_scanning = true;
}

void scan_stop() {
  if (!g_scanning)
    return;
  // 成功すれば GAP_EVENT_INQUIRY_COMPLETE が来て finish_scan(1)。止められない状態
  // (すでに終わっている) ならここで閉じる。
  // ★stop の中で COMPLETE が同期的に出る場合 (開始前の stop) があるので、先に印を付ける。
  g_scan_stopping = true;
  if (gap_inquiry_stop() != 0)
    finish_scan(1, 0);
}

void forget(const uint8_t *addr_or_null) {
  if (addr_or_null == nullptr) {
    drop_connection("forget all");
    gap_delete_all_link_keys();
    g_target_set = false;
    BOARD::diag_printf("[BT_CLASSIC] forgot all link keys, target cleared\n");
    return;
  }
  bd_addr_t addr;
  memcpy(addr, addr_or_null, 6);
  if (is_target(addr)) {
    drop_connection("forget target");
    g_target_set = false;
  }
  gap_drop_link_key_for_bd_addr(addr);
  BOARD::diag_printf("[BT_CLASSIC] forgot %02x:%02x:%02x:%02x:%02x:%02x\n", addr[0],
                     addr[1], addr[2], addr[3], addr[4], addr[5]);
}

void list_bonds() {
  if (!g_hci_working) {
    g_list_pending = true; // WORKING のイベントで返す
    return;
  }
  uint8_t count = 0;
  btstack_link_key_iterator_t it;
  if (gap_link_key_iterator_init(&it)) {
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    while (count < kMaxBondsReported &&
           gap_link_key_iterator_get_next(&it, addr, key, &type)) {
      emit(RX_BOND, addr, 6);
      ++count;
    }
    gap_link_key_iterator_done(&it);
  } else {
    BOARD::diag_printf("[BT_CLASSIC] link key iterator unavailable\n");
  }
  emit(RX_BOND_END, &count, 1);
}

void packet_handler(uint8_t packet_type, uint16_t, uint8_t *packet, uint16_t) {
  if (packet_type != HCI_EVENT_PACKET)
    return;
  switch (hci_event_packet_get_type(packet)) {
  case BTSTACK_EVENT_STATE:
    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
      // 実績のある形: 最初の接続は HCI_STATE_WORKING のイベントの中で直接呼ぶ。
      g_hci_working = true;
      if (g_list_pending) {
        g_list_pending = false;
        list_bonds();
      }
      connect_now();
    }
    break;

  case GAP_EVENT_INQUIRY_RESULT: {
    if (!g_scanning || g_scan_stopping)
      break;
    uint8_t d[9];
    bd_addr_t addr;
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    memcpy(d, addr, 6);
    const uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
    d[6] = (uint8_t)(cod >> 16);
    d[7] = (uint8_t)(cod >> 8);
    d[8] = (uint8_t)cod;
    emit(RX_SCAN_FOUND, d, sizeof(d));
    break;
  }

  case GAP_EVENT_INQUIRY_COMPLETE:
    // 止めた場合も、時間切れの場合もここへ来る。scan 中でなければ (開始前の stop の
    // 余波など) 無視する。
    finish_scan(g_scan_stopping ? 1 : 0, 0);
    break;

  case HCI_EVENT_PIN_CODE_REQUEST: { // 旧式ペアリングの互換機
    bd_addr_t addr;
    hci_event_pin_code_request_get_bd_addr(packet, addr);
    if (is_target(addr))
      gap_pin_code_response(addr, "0000");
    else
      hci_send_cmd(&hci_pin_code_request_negative_reply, addr);
    break;
  }

  case HCI_EVENT_USER_CONFIRMATION_REQUEST: { // SSP Just Works: 対象だけ承認
    bd_addr_t addr;
    hci_event_user_confirmation_request_get_bd_addr(packet, addr);
    if (is_target(addr)) {
      gap_ssp_confirmation_response(addr);
    } else {
      BOARD::diag_printf("[BT_CLASSIC] SSP from non-target rejected\n");
      gap_ssp_confirmation_negative(addr);
    }
    break;
  }

  // LINK_KEY_REQUEST / LINK_KEY_NOTIFICATION は BTstack 本体 (TLV の link key DB) に
  // 任せる。ここで否定応答すると保存済みの鍵が使われず毎回再ペアリングになる。

  case HCI_EVENT_AUTHENTICATION_COMPLETE: {
    const uint8_t status = hci_event_authentication_complete_get_status(packet);
    if (status == ERROR_CODE_AUTHENTICATION_FAILURE ||
        status == ERROR_CODE_PIN_OR_KEY_MISSING) {
      // コントローラ側が鍵を持たない (シンクで新規ペアリング) と失敗する。古い鍵で
      // 認証し続けないよう対象の鍵を捨て、次回は SSP をやり直す。
      if (g_target_set)
        gap_drop_link_key_for_bd_addr(g_target);
      BOARD::diag_printf("[BT_CLASSIC] auth failed 0x%02x, dropped link key\n",
                         status);
    }
    break;
  }

  case HCI_EVENT_HID_META:
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
    case HID_SUBEVENT_INCOMING_CONNECTION: {
      const uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(packet);
      bd_addr_t addr;
      hid_subevent_incoming_connection_get_address(packet, addr);
      if (!is_target(addr) || g_open || g_scanning) {
        BOARD::diag_printf("[BT_CLASSIC] incoming declined (target=%d open=%d scan=%d)\n",
                           (int)is_target(addr), (int)g_open, (int)g_scanning);
        hid_host_decline_connection(cid);
        break;
      }
      const uint8_t rc = hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT);
      BOARD::diag_printf("[BT_CLASSIC] accept incoming rc=0x%02x\n", rc);
      if (rc == ERROR_CODE_SUCCESS) {
        g_cid = cid;
        g_connecting = true;
        arm_timer(kConnectingGuardMs);
      }
      break;
    }
    case HID_SUBEVENT_CONNECTION_OPENED: {
      const uint8_t status = hid_subevent_connection_opened_get_status(packet);
      g_connecting = false;
      if (status != ERROR_CODE_SUCCESS) {
        BOARD::diag_printf("[BT_CLASSIC] open failed 0x%02x\n", status);
        emit(RX_CLOSED, &status, 1);
        schedule_retry();
        break;
      }
      g_open = true;
      g_retry_ms = kRetryMinMs;
      disarm_timer();
      g_cid = hid_subevent_connection_opened_get_hid_cid(packet);
      hid_host_send_set_protocol_mode(g_cid, HID_PROTOCOL_MODE_REPORT);
      emit(RX_OPENED, g_target, 6);
      BOARD::diag_printf("[BT_CLASSIC] HID opened cid=%04x\n", g_cid);
      break;
    }
    case HID_SUBEVENT_CONNECTION_CLOSED: {
      g_open = false;
      const uint8_t reason = 0;
      emit(RX_CLOSED, &reason, 1);
      schedule_retry();
      break;
    }
    case HID_SUBEVENT_REPORT:
      emit(RX_REPORT, hid_subevent_report_get_report(packet),
           (uint8_t)hid_subevent_report_get_report_len(packet));
      break;
    case HID_SUBEVENT_SET_REPORT_RESPONSE: {
      const uint8_t st = (uint8_t)hid_subevent_set_report_response_get_handshake_status(packet);
      emit(RX_SET_REPORT_RESP, &st, 1);
      break;
    }
    default:
      break;
    }
    break;

  default:
    break;
  }
}

} // namespace

void init(void (*emit_fn)(const hid_frame &)) {
  g_emit = emit_fn;
  btstack_run_loop_set_timer_handler(&g_timer, &timer_handler);
  sdp_init();
  hid_host_init(g_descriptor_storage, sizeof(g_descriptor_storage));
  hid_host_register_packet_handler(&packet_handler);
  g_hci_reg.callback = &packet_handler;
  hci_add_event_handler(&g_hci_reg);

  // 鍵とペアリングは BTstack 標準に任せる (link key は SDK が張る TLV)。
  gap_set_local_name("RC Car");
  gap_set_class_of_device(0x002508); // Peripheral / Gamepad
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_auto_accept(0); // 承認は対象アドレスだけ (packet_handler)
  gap_connectable_control(1); // 保存済みコントローラのボタン押下再接続を受ける
  gap_discoverable_control(0);
}

void on_command(const hid_frame &f) {
  switch (f.kind) {
  case TX_CONNECT:
    if (f.len < 6)
      return;
    if (!g_target_set || memcmp(g_target, f.data, 6) != 0) {
      memcpy(g_target, f.data, 6);
      g_target_set = true;
    }
    if (g_scanning) { // スキャン中の CONNECT = 見つけた相手へ。inquiry を止めて COMPLETE で接続
      scan_stop();
      break;
    }
    connect_now();
    break;
  case TX_FORGET:
    forget(f.len >= 6 ? f.data : nullptr);
    break;
  case TX_SCAN_START:
    scan_start(f.len >= 1 ? f.data[0] : 30);
    break;
  case TX_SCAN_STOP:
    scan_stop();
    break;
  case TX_LIST_BONDS:
    list_bonds();
    break;
  case TX_DISCONNECT:
    if (g_cid != 0 && (g_open || g_connecting))
      hid_host_disconnect(g_cid);
    break;
  case TX_SET_REPORT:
    if (g_open && f.len >= 1) {
      const uint8_t rc = hid_host_send_set_report(
          g_cid, HID_REPORT_TYPE_OUTPUT, f.data[0], &f.data[1], f.len - 1u);
      if (rc != ERROR_CODE_SUCCESS)
        BOARD::diag_printf("[BT_CLASSIC] set_report rc=0x%02x\n", rc);
    }
    break;
  default:
    break;
  }
}

void on_classic_disconnect(uint8_t reason) {
  BOARD::diag_printf("[BT_CLASSIC] ACL disconnected reason=0x%02x\n", reason);
  if (g_open || g_connecting) {
    g_open = false;
    emit(RX_CLOSED, &reason, 1);
    schedule_retry();
  }
}

} // namespace bt_classic
} // namespace objects
} // namespace shizuku
