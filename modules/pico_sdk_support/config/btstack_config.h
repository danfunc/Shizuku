#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// BLE UART (Nordic UART Service) 専用の最小構成。旧 CMake ビルドの
// include/btstack_config.h は Classic/SDP/HID も有効化しているが、あれは
// このリポジトリの別機能 (BLE と無関係) 向けの設定で、そのまま持ち込むと
// Classic 側のシンボル (de_get_len 等、TLV link key db 等) が未定義参照になる
// (実測)。BLE peripheral (NUS) だけなら Classic は要らない。
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
// Numeric Comparison は LE Secure Connections (LESC) 専用の方式。これを定義
// しないと Legacy ペアリングに落ち、MITM 必須のため Passkey Entry になる。
#define ENABLE_LE_SECURE_CONNECTIONS
#define MAX_NR_GATT_CLIENTS 0
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1
#define NVM_NUM_DEVICE_DB_ENTRIES 1

// ★ CYW43 の実用上限に合わせる (旧実装の実測知見をそのまま踏襲、変更しない
//   こと — memory note "bt-wedge-needs-power-cycle" 参照)。ACL が 1 LL PDU
//   (251B) を超える分割送信になると、切断/再接続を繰り返した末に BT コアが
//   完全無応答になる (btstack #654, pico-sdk #2181 と同一症状)。
//   ACL ペイロードを 247+4=251 に抑えると ATT MTU=247 / notify データ 244B と
//   なり、フルサイズ notify がちょうど 1 LL パケットに収まって分割が消える。
#define HCI_ACL_PAYLOAD_SIZE (247 + 4)
#define HCI_OUTGOING_PRE_BUFFER_SIZE 64
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 64

#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_SERVICES 2
#define MAX_NR_L2CAP_CHANNELS 2

#define HAVE_MALLOC
#define HAVE_ASSERT
#define HAVE_EMBEDDED_TIME_MS

#endif // BTSTACK_CONFIG_H
