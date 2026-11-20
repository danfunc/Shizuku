// BLE peripheral (NUS) + Classic HID host の和集合。config/btstack_config.h (BLE のみ) の
// 項目はそのまま、Classic に要る分だけ足す。設計: bt-core-port/DESIGN.md
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// pico-sdk の PICO_BT_ENABLE_CLASSIC が定義する場合があるので一度外してから 1 に固定。
#ifdef ENABLE_CLASSIC
#undef ENABLE_CLASSIC
#endif
#define ENABLE_CLASSIC 1
#define ENABLE_HID_HOST
#define ENABLE_SDP
#define ENABLE_SDP_QUERIES

#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_SECURE_CONNECTIONS
#define MAX_NR_GATT_CLIENTS 0
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1
#define NVM_NUM_DEVICE_DB_ENTRIES 1

// Classic の link key (1 台) + 余裕 1。
#define NVM_NUM_LINK_KEYS 2
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2

// Classic の SDP 応答は 247 を超えるので Classic の値に合わせる (LE の DLE 247 は包含)。
#define HCI_ACL_PAYLOAD_SIZE 1021
#define HCI_OUTGOING_PRE_BUFFER_SIZE 64
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 64

// 同時接続: LE (NUS) 1 + Classic (HID) 1。
#define MAX_NR_HCI_CONNECTIONS 2
// L2CAP: ATT/SM の固定チャネル + HID の control/interrupt + SDP。
#define MAX_NR_L2CAP_SERVICES 4
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_SDP_CLIENTS 1
#define MAX_NR_HID_HOST_CONNECTIONS 1

#define HAVE_MALLOC
#define HAVE_ASSERT
#define HAVE_EMBEDDED_TIME_MS

#endif // BTSTACK_CONFIG_H
