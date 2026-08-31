#ifndef SHIZUKU_OBJECTS_BLE_UART_HPP
#define SHIZUKU_OBJECTS_BLE_UART_HPP
#include <cstdint>
#include "shizuku/objects/gdb_stub.hpp"

namespace shizuku {
namespace objects {
namespace ble_uart {

// 1 レコード (244B は BLE ATT MTU 247 - ATT ヘッダ 3 由来)
struct ble_frame {
  uint16_t len;
  uint8_t data[244];
};

using frame_t = ble_frame;
constexpr uintptr_t NO_STREAM = (uintptr_t)-1;

// ペアリング施錠まわりの外向き状態。★シェル (別スレッド) が読むための
//   **写し** であって、btstack の内部状態そのものではない。
//   [[no-btstack-from-caller-thread]] のとおり、btstack へ触ってよいのは
//   ble_uart の poll スレッドだけなので、外から見える口は「poll ループが
//   毎周更新する平たい変数を読む」形にしてある。
struct pairing_state {
  // NC 要求の通し番号。★「今 pending か」ではなく**通し番号**にしたのは、
  //   シェルが取りこぼさずに「新しい問いかけが来た」と判定できるようにするため。
  //   pending 旗だけだと、シェルのポーリング間隔 (200ms) の間に要求が出て
  //   消えた場合に一度も表示されない。
  uint32_t nc_generation;
  uint32_t nc_passkey;         // 表示すべき 6 桁 (nc_pending のときだけ意味を持つ)
  uint32_t strikes;            // 直近の失敗ペアリング回数 (クールダウンの元)
  uint32_t allow_seconds_left; // 一時解錠の残り秒 (0 = 解錠していない)
  uint32_t block_seconds_left; // DoS クールダウンの残り秒 (0 = 広告中)
  uint8_t nc_pending;          // 1 = 承認待ち
  uint8_t locked;              // 1 = 施錠中 (新規ペアリングを拒否する)
  uint8_t bonded;              // 保存済みボンド件数
  uint8_t connected;           // 1 = リンクあり
  uint8_t authorized;          // 1 = 暗号化・認証済み (コマンドを受け付ける)
  uint8_t reserved[3];
};

enum struct method : uintptr_t {
  MAIN = 0,
  GET_RX_STREAM = 1,
  SET_TX_STREAM = 2,
  POLL = 3,
  SET_GDB_STREAMS = 4,
  GET_OTA_STREAM = 5,
  REQUEST_DISCONNECT = 6,
  GET_CH2_RX_STREAM = 7,
  SET_CH2_TX_STREAM = 8,
  // a1 = pairing_state*。呼び出し元スレッドで安全に読めるよう、写しを詰める。
  GET_PAIRING_STATE = 9,
  // a1: 1 = 承認 / 0 = 拒否。★旗を立てるだけ (btstack は poll ループが叩く)。
  PAIRING_ANSWER = 10,
  // a1: 1 = 施錠 / 0 = **一度だけ**解錠 (PAIRING_ALLOW_WINDOW_S で自動失効)。
  // ★「解錠モード」ではなく一回券にしてあるのは、戻し忘れを構造的に潰すため。
  SET_PAIRING_LOCK = 11,
  // 保存済みボンドを全部消す。★締め出されたときの最後の逃げ道 (有線 UART から)。
  FORGET_BONDS = 12,
};

// オブジェクトを登録 (任意のオブジェクトIDを指定可能)
uint32_t register_ble_uart(uintptr_t object_id);

// poll スレッドを起動
uint32_t start_ble_uart(uintptr_t object_id);

} // namespace ble_uart
} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_BLE_UART_HPP
