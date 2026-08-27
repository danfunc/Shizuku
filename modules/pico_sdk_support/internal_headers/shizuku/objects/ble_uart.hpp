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
};

// オブジェクトを登録 (任意のオブジェクトIDを指定可能)
uint32_t register_ble_uart(uintptr_t object_id);

// poll スレッドを起動
uint32_t start_ble_uart(uintptr_t object_id);

} // namespace ble_uart
} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_BLE_UART_HPP
