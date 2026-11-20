#ifndef SHIZUKU_OBJECTS_BT_CLASSIC_HPP
#define SHIZUKU_OBJECTS_BT_CLASSIC_HPP
// BT core の Classic HID host サービス。★全関数が CYW43/BTstack の owner スレッド
// (ble_uart の poll_loop) からだけ呼ばれる。外のオブジェクトは 2 本の STREAM
// (hid_rx / hid_tx, ble_uart のメソッド GET_HID_RX_STREAM / SET_HID_TX_STREAM) で話す。
#include <cstdint>

namespace shizuku {
namespace objects {
namespace bt_classic {

struct hid_frame {
  uint8_t kind;
  uint8_t len;
  uint8_t data[70];
};

// owner -> 外 (hid_rx)
enum rx_kind : uint8_t {
  RX_OPENED = 1,          // HID 接続完了。data[0..5] = 相手の BD_ADDR
  RX_CLOSED = 2,          // data[0] = 理由 (HCI reason か opened の失敗 status)
  RX_REPORT = 3,          // data = HID_SUBEVENT_REPORT の生データ (0xA1, report id, ...)
  RX_SET_REPORT_RESP = 4, // data[0] = handshake status
  // ---- 登録し直し (bond 管理 / スキャン)。設計: DESIGN.md「コントローラの登録し直し」 ----
  RX_SCAN_FOUND = 5, // data[0..5] = BD_ADDR, data[6..8] = CoD (上位バイトが先: 0x00,0x25,0x08)。
                     // 同じ相手が繰り返し来ることがある (重複排除は外)。
  RX_SCAN_DONE = 6,  // data[0] = 0: inquiry 時間切れ / 1: TX_SCAN_STOP か TX_CONNECT で止めた /
                     //           2: 開始失敗 (data[1] = BTstack の status)
  RX_BOND = 7,       // data[0..5] = 保存済み link key を持つ相手の BD_ADDR (TX_LIST_BONDS の返事)
  RX_BOND_END = 8,   // TX_LIST_BONDS の返事の終端。data[0] = RX_BOND の個数
};

// 外 -> owner (hid_tx)
enum tx_kind : uint8_t {
  TX_CONNECT = 1,    // data[0..5] = 接続先 BD_ADDR (この 1 台だけを受け付ける)
  TX_DISCONNECT = 2,
  TX_SET_REPORT = 3, // data[0] = report id, data[1..len) = payload (OUTPUT report)
  // ---- 登録し直し ----
  TX_FORGET = 4,     // len=0: link key を全消去 / len>=6: その相手の鍵だけ消去。
                     //   接続中・接続試行中なら切断し、(全消去か、その相手が target なら) target を解除する。
  TX_SCAN_START = 5, // data[0] = 秒数 (1..61 に丸める)。Classic inquiry を始め、見つけた相手ごとに
                     //   RX_SCAN_FOUND、終わりに RX_SCAN_DONE。スキャン中は自動再接続を止め、
                     //   接続中なら切断する。target は保持 (TX_FORGET してから呼ぶと未設定のまま)。
  TX_SCAN_STOP = 6,  // inquiry を止める (RX_SCAN_DONE(1) が来る)。target があれば再接続を再開。
  TX_LIST_BONDS = 7, // 保存済み相手を RX_BOND で列挙し、RX_BOND_END で終える。
};

// btstack 初期化後・hci_power_control(ON) の前に呼ぶ。emit は owner スレッドから
// 呼ばれ、フレームを hid_rx へ積む (満杯なら最古を捨てる実装)。
void init(void (*emit)(const hid_frame &));
void on_command(const hid_frame &frame);
// LE 以外の HCI 切断 (= Classic) が来たとき。
void on_classic_disconnect(uint8_t reason);

} // namespace bt_classic
} // namespace objects
} // namespace shizuku
#endif
