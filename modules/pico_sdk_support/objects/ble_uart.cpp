// ===========================================================================
//  BLE UART (Nordic UART Service) — Shizuku オブジェクト版 (v1: 最小構成)
// ===========================================================================
//  移植元: flight_robocon_telemetory_sender/BLE_UART_DRIVER.cpp (旧 CMake ビルド)。
//  v1 スコープ: advertise / pairing / notify・write / CI 強制 (15ms 固定)。
//  ★ペアリング方式は CDC の実行時コマンドで切替 (既定 = NC 必須、事故防止)。
//    'A'+Enter で自動ペアリング (Just Works) へ、'S'+Enter で NC 必須へ戻す
//    (g_auto_pair / apply_security_mode 参照)。
//
//  ★今回見送ったもの (旧実装にはあるが、ここには無い):
//    - 接続ウォッチドッグ (生存兆候途絶での強制切断) と BT 電源再投入による
//      無応答復旧 (bt_full_chip_restart)
//    - TX **優先度付き**マルチストリーム (旧 register_tx_stream)。合流自体は
//      flight_controller (ハブ) が持つ形になったが、優先度はまだ無い
//      (ハブはラウンドロビン)。付けるならハブの中だけを直せばよい
//    - RSSI 周期計測 / 2M PHY 要求 / poll 時間計装などの診断機能
//  これらが要るとわかったら、旧実装のロジックはそのまま参照できる。
//
//  ★ペアリング(bonding)鍵の flash 永続化 — **このファイルは何もしない**。
//    pico-sdk の `btstack_cyw43_init()` (= `cyw43_arch_init()` から呼ばれる) が
//    `setup_tlv()` の中で flash bank の TLV を張り、`le_device_db_tlv_configure`
//    まで済ませている (SDK の btstack_cyw43.c)。**BT 有効でこの経路を使う限り、
//    bonding の永続化は最初から有効**だった。
//
//    ★ここへ辿り着くまでに 4 回自前実装を試して全部壊した (2026-08-24)。
//      1. flash_fs へ自前ブリッジ → PANIC
//      2. pico_flash_bank_instance() + グローバル extra-header でオフセット
//         上書き → `assertion "false" failed` (i2c 登録直後)
//      3. 自前 hal_flash_bank_t → 起動直後にハング
//      4. flash_fs を外して pico_flash_bank_instance() を既定オフセットで使用
//         → 一見成功 (再起動後の reencryption を実機確認) したが、**SDK と
//         二重に TLV を張っていた**ため後述の形で板が起動不能になった。
//    2 と 3 の「原因不明」も、今にして思えば同じ二重初期化とその後始末
//    (バンク erase → flash_safe_execute) が正体だった可能性が高い。
//
//    ★★踏み抜いた壊れ方 (再発させないこと):
//      同じ物理バンクを SDK と自前の 2 つの `btstack_tlv_flash_bank_t` が
//      別々に管理 → バンクの整合が崩れる → 次回起動の
//      `btstack_tlv_flash_bank_init_instance` が「消してやり直す」判断 →
//      その erase が `flash_safe_execute` を呼ぶ → **cyw43_arch_init の中で
//      core1 はまだ起きていない**ので `multicore_lockout_victim_is_initialized(1)`
//      が false → `assert(false)` (pico_flash/flash.c:190) で停止。
//      壊れた状態は flash に残るので、**電源再投入でもアプリの焼き直しでも
//      直らない**。復旧は `picotool erase --all` のみ。
//
//  ★接続フリーズの根本原因 (旧実装の教訓) は printf の二重ブロッキングだった
//    ("ble-freeze-is-blocking-printf")。Shizuku 側は usb_cdc.cpp が診断出力を
//    満杯時ブロックせず捨てる設計に既になっているので、ここでは何もしない
//    (Phase B 参照)。
#include "btstack_config.h"
extern "C" {
#include "ble/att_db.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h" // ボンド件数の確認 / 鍵の削除 (施錠と解錠)
#include "btstack.h"
#include "gap.h"
#include "hci.h"
#include "pico/stdlib.h" // getchar_timeout_us (USB CDC 経由の NC confirm 入力)
}
#include "ble_uart.h" // pico_btstack_make_gatt_header() が生成
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/ble_uart.hpp"
#include "shizuku/objects/gdb_stub.hpp" // GDB リンクをストリームで運ぶ
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/stream.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pico/cyw43_arch.h> // cyw43_arch_init / cyw43_arch_poll

namespace shizuku {
namespace objects {

#if defined(CYW43_WL_GPIO_LED_PIN)
// peripherals オブジェクトがリンクされていないバイナリでもリンクを通すための
// weak 定義
__attribute__((weak)) void led_sync_hw() {}
__attribute__((weak)) void led_toggle() {}
#else
__attribute__((weak)) void led_toggle() {}
#endif

namespace ble_uart {

static uintptr_t s_object_id = 7;

namespace {

using ARCH = shizuku::KERNEL::ARCH;
using BOARD = shizuku::KERNEL::BOARD;

// ★ペアリング方式は CDC(診断チャネル)の専用コマンドで実行時に切り替える
//   (ビルドを分けない)。既定は **NC 必須 (安全側、事故防止)**。
//   'A' + Enter でテスト用の自動ペアリング (Just Works, MITM 無し) へ、
//   'S' + Enter で NC 必須へ戻す。切替は次回以降のペアリング交渉に効く
//   (今まさに進行中の交渉には効かない — sm_set_* は次の手続き開始時に
//   読まれる値を変えるだけなので)。
static bool g_auto_pair = false;

// ===========================================================================
//  ペアリング施錠 (2026-08-31)
// ===========================================================================
//  ★狙い: 「一度ボンドしたら、以後の新規ペアリングは拒む」。
//    なぜ必要か — btstack_config.h の `MAX_NR_LE_DEVICE_DB_ENTRIES` は **1**。
//    つまり新しいペアリングが成立すると、**こちらの鍵が押し出されて消える**。
//    攻撃者が勝手にペアリングを通せば、機体を奪われるだけでなく、正規の
//    母艦が締め出されて OTA (唯一の遠隔書き込み手段) も失われる。
//
//  ★どこで拒むか: `sm_set_accepted_stk_generation_methods(0)`。
//    btstack は Pairing Request を受けた直後 (sm.c の `sm_stk_generation_init`)
//    でこの集合を検査し、空なら `SM_REASON_AUTHENTHICATION_REQUIREMENTS` で
//    即座に落とす。**公開鍵交換 (ECDH) より前**なので、拒否そのものが安く、
//    ペアリング要求を投げ続けられても計算資源を食われない (DoS 側にも効く)。
//    ★★**再暗号化 (reencryption) はこの経路を通らない** — 保存済み LTK を
//      使う手続きで Pairing Request が飛ばないため。だから施錠しても
//      「既にボンドしている母艦の再接続」は素通しで、OTA 経路は壊れない。
//
//  ★捨てた案:
//    - `sm_bonding_decline()` を SM_EVENT_PAIRING_STARTED で呼ぶ:
//      **効かない**。sm.c の decline は `SM_PH1_W4_USER_RESPONSE` 等の
//      「ユーザ応答待ち」状態でしか作用せず、PAIRING_STARTED の時点では
//      default: break で無視される (SDK 2.2.0 の sm.c:5367 を読んで確認)。
//      ただしユーザ応答イベント (NC / Just Works) での decline は効くので、
//      **二重の網**として下の SM ハンドラにも入れてある。
//    - `sm_set_authentication_requirements` をいじる: 要求の中身を変えるだけで
//      「拒む」意思にならない。相手が条件を満たせば通ってしまう。
//
//  ★締め出し対策 (ここを間違えると機体が文鎮になる):
//    母艦側でボンドを消した (macOS の「このデバイスを削除」等) 場合、
//    向こうは新規ペアリングを試みる → 施錠中なので拒否 → **BLE から一切
//    入れなくなる**。逃げ道は有線の XIAO (UART0) からの
//    `nc allow` (一回だけ許可) と `nc forget!` (鍵を全部消す)。
//    有線経路を持たない解錠手段は用意しない — 用意すると施錠の意味が消える。
// ★「施錠 ON/OFF の旗」は**置かない**。施錠は
//     鍵が 1 本でもある && 一回券が生きていない
//   という状態そのもので表す。旗にすると「OFF のまま戻し忘れて飛ばす」形が
//   作れてしまい、しかもそれが**無言で**成立する。状態で表せば、解錠は
//   必ず「期限付きの一回券」の形しか取れない。
static uint64_t g_pair_allow_until_us = 0;  // 一回券の期限 (0 = 券なし)
static uint32_t g_pair_strikes = 0;         // 失敗ペアリングの連続回数
static uint64_t g_pair_block_until_us = 0;  // クールダウン (広告停止) の期限
static uint32_t g_bonded_count = 0;         // le_device_db_count() の写し
static uint32_t g_nc_generation = 0;        // NC 要求の通し番号
static uint32_t g_nc_passkey = 0;
// 外 (シェル) からの依頼。★旗だけ立てて btstack を触るのは poll ループ
//   ([[no-btstack-from-caller-thread]])。
static volatile uint8_t g_pair_answer_requested = 0; // 0=無 1=承認 2=拒否
static volatile bool g_forget_bonds_requested = false;
static volatile bool g_pair_mode_dirty = false;      // 施錠状態を再適用したい

// 一回券の有効時間。★短すぎると人が間に合わず、長すぎると「解錠したまま
//   忘れて飛ばす」が起きる。人が板の横に居て操作している前提で 3 分。
static constexpr uint64_t PAIRING_ALLOW_WINDOW_US = 180000000ull;
// 失敗が続いたら広告を止める閾値と時間 (後述の DoS 対策)。
static constexpr uint32_t PAIR_STRIKE_LIMIT = 3;
static constexpr uint64_t PAIR_BLOCK_US = 60000000ull;
// 認可されないまま繋ぎっぱなしのリンクを畳むまでの時間。
// ★BLE の接続は 1 本しか持てないので、**繋いで黙っているだけ**で正規の母艦を
//   締め出せてしまう。これが実際にいちばん安い DoS。NC の承認待ち中は
//   人間の操作時間なので、この計測を止める (下の unauth_deadline_active 参照)。
//   ★90 秒は「実測で決めていない」値。実機で正規の初回ペアリングに何秒
//     かかるかを測ってから詰めること (下げると占拠に強く、誤爆に弱くなる)。
static constexpr uint64_t UNAUTH_LINK_TIMEOUT_US = 90000000ull;

// 施錠が効いているか。★ボンドが 0 件のときは施錠しない — 初回ペアリングまで
//   塞いでしまうと、工場出荷状態の板に誰も入れなくなる (卵と鶏)。
static bool pairing_locked_now(uint64_t now_us) {
  if (g_bonded_count == 0)
    return false;
  if (g_pair_allow_until_us != 0 && now_us < g_pair_allow_until_us)
    return false; // 一回券が生きている
  return true;
}

static void apply_security_mode() {
  // ★施錠は「受け入れる STK 生成方式」の集合で表す。空集合 = どの方式でも
  //   ペアリングを受けない。ここを毎回上書きするので、解錠したら必ず戻る。
  if (pairing_locked_now(BOARD::time_us())) {
    sm_set_accepted_stk_generation_methods(0);
  } else {
    sm_set_accepted_stk_generation_methods(
        SM_STK_GENERATION_METHOD_JUST_WORKS |
        SM_STK_GENERATION_METHOD_NUMERIC_COMPARISON |
        SM_STK_GENERATION_METHOD_PASSKEY | SM_STK_GENERATION_METHOD_OOB);
  }
  if (g_auto_pair) {
    // Just Works: MITM を要求しないので NC (Numeric Comparison) 自体が
    // 発生せず、OS 側の確認ダイアログも Pico 側の y/n 入力も無しで
    // ペアリング〜暗号化まで自動的に終わる (macOS の CoreBluetooth は
    // NC の承認をアプリ側から自動化する API を提供していないため、
    // ヘッドレスなテストで確実に自動化する手段は実質これだけ)。
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION |
                                       SM_AUTHREQ_BONDING);
  } else {
    // LE Secure Connections + Numeric Comparison (既定、事故防止)。
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_YES_NO);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION |
                                       SM_AUTHREQ_MITM_PROTECTION |
                                       SM_AUTHREQ_BONDING);
  }
}

struct call_result {
  uintptr_t error;
  uintptr_t value;
};

call_result api(shizuku::object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uintptr_t export_method(method m, uintptr_t entry) {
  return api(shizuku::object_api::EXPORT_METHOD, (uintptr_t)m, entry).error;
}

// ---- セキュリティ / 接続状態 (旧実装 §「セキュリティ設定」「接続/通知状態」) ----
static hci_con_handle_t nc_pending_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
// リンクが張られた時刻。認可されないまま居座る相手を畳むのに使う。
static uint64_t conn_started_us = 0;
static bool tx_notify_enabled = false;
static bool can_send_requested = false;
static uint16_t conn_interval = 0;
// SM の判定 (暗号化+認証) を反映するだけ。独自の秘密は持たない (fail-closed)。
static bool cmd_authorized = false;

// ---- RX: Shizuku ストリームとして外へ出す (shell 的な解釈はしない) ----------
shizuku::stream::storage<frame_t, 8> g_rx;
uintptr_t g_rx_stream_id = 0;

// ---- TX: ble_uart 自身の行 (内部専用、ストリーム登録しない) -------------------
shizuku::stream::storage<frame_t, 16> g_tx;

// ---- TX: ハブ (flight_controller) から流れてくる本線 --------------------------
// ★実体を持つのは向こう側。こちらは番号で引いて (STREAM_OPEN) consumer 席に
//   座るだけ。番号は合成側が起動前に SET_TX_STREAM で渡す。
uintptr_t g_tx_in_id = NO_STREAM;
shizuku::stream::handle<frame_t> g_tx_in;

// ---- GDB リンク (RSP) --------------------------------------------------------
// ★NUS とは別の characteristic で運ぶ (ble_uart.gatt 参照)。ここは**運ぶだけ** —
//   RSP の中身は一切見ない (解釈するのは Shizuku の gdb server)。
using link_chunk = shizuku::objects::link_chunk;
uintptr_t g_gdb_to_stub_id = NO_STREAM;   // ここから stub へ (host の書き込み)
uintptr_t g_gdb_from_stub_id = NO_STREAM; // stub からここへ (GDB への返事)
shizuku::stream::handle<link_chunk> g_gdb_to_stub;
shizuku::stream::handle<link_chunk> g_gdb_from_stub;
bool gdb_notify_enabled = false;
// notify のクレジット切れで出せなかった 1 個をここに留める (下の flush_gdb 参照)。
link_chunk g_gdb_held{};
bool g_gdb_held_valid = false;

// ---- OTA 受信 ----------------------------------------------------------------
// ★ここも**運ぶだけ**。中身 (ヘッダ/CRC/イメージ) は ota オブジェクトが見る。
// ★容量を大きめに取る: 転送中は BLE から連続で流れ込み、ota 側は flash 書き
//   込み (1 セクタ消去に数十ms) で待たされるため、ここが詰まると取りこぼす。
//   取りこぼしは CRC で必ず検出されるが、やり直しは 30 秒単位で高い。
shizuku::stream::storage<frame_t, 32> g_ota_rx;
uintptr_t g_ota_stream_id = 0;
// ---- 拡張チャネル #2 (6E403001-...) ----
shizuku::stream::storage<frame_t, 16> g_ch2_rx;
uintptr_t g_ch2_rx_stream_id = 0;
uintptr_t g_ch2_tx_in_id = NO_STREAM;
shizuku::stream::handle<frame_t> g_ch2_tx_in;
bool ch2_notify_enabled = false;
bool g_ch2_held_valid = false;
frame_t g_ch2_held{};

// 「デバッガが繋がっている」= notify が有効 **かつ** リンクが認可済み。
// ★認可を必ず条件に入れる — GDB は任意のメモリ読み書きとレジスタ操作そのもの
//   なので、暗号化・ボンディングされていないリンクに開けてはいけない
//   (fail-closed。RX コマンドと同じ扱い)。
static uint32_t tx_pending(); // 下で定義 (GDB CCC 有効時の補填で先に要る)

// ★★「デバッガが繋がっている」= notify 有効 + 認可済み + **実際に RSP が
//   来ている**。3 つ目を足したのは 2026-08-24:
//     macOS は bonding した相手の CCC を覚えていて、**こちらが購読して
//     いなくても再接続で notify を張り直してくる**。OTA しかしていないのに
//     `gdb notify enabled` が毎回出るのはこれ。
//   これを「繋がった」と数えると、スタブは寝るのをやめて RSP を待つ空回りに
//   入り (受信 → 失敗 → YIELD の繰り返し)、**core0 を BLE の poll と食い合う**。
//   実測で OTA の転送が 29% で止まった。
//   ★1 バイトでも RSP が来れば本物なので、それを条件にする。来る前は
//     スタブが寝ているが、バイトはストリームに溜まるので取りこぼさない
//     (スタブは起きたときに読む)。
static bool gdb_saw_traffic = false;

static void update_gdb_connected() {
  shizuku::objects::gdb_link_set_connected(gdb_notify_enabled &&
                                           cmd_authorized && gdb_saw_traffic);
}

// ★このリングは **ble_uart 自身の行専用** (接続状態などの内部メッセージ)。
//   push するのも pop するのも poll スレッド 1 本だけなので、SPSC が自明に
//   成り立つ (そもそもストリームとして登録もしない = 席の話にすらならない)。
//   外部からの送信は**ここへ入ってこない** — 送り手は自前のストリームを持ち、
//   flight_controller (ハブ) が束ねた 1 本を g_tx_in として下で消費する (D46)。
//   かつては SEND 経由で bno055 / bme280 もここへ push しており、producer が
//   3 つになって「同じ wr を 2 スレッドが読んで同じスロットを潰す」= 無音で
//   レコードが消える状態だった (割り込み禁止で塞いでいたが、それは
//   「全 producer が同じコアに居る」ことに依存する繋ぎでしかなかった)。
static void tx_push(const uint8_t *data, uint32_t len) {
  if (len > sizeof(frame_t::data))
    len = sizeof(frame_t::data); // v1: 分割送信はしない (呼び出し側が切る)
  frame_t f{};
  f.len = (uint16_t)len;
  memcpy(f.data, data, len);
  g_tx.hdl().push(f); // LOSSLESS ではない (v16, 満杯なら最古を上書き)
}

static void tx_push_line(const char *s) { tx_push((const uint8_t *)s, (uint32_t)strlen(s)); }

// ---- 広告データ (旧実装のまま) ----------------------------------------------
static uint8_t adv_buffer[31];
static uint8_t adv_len = 0;

static void build_adv_data() {
  uint8_t *p = adv_buffer;
  *p++ = 0x02;
  *p++ = BLUETOOTH_DATA_TYPE_FLAGS;
  *p++ = 0x06;
  static const char name[] = "Shizuku UART";
  uint8_t name_len = (uint8_t)strlen(name);
  *p++ = (uint8_t)(name_len + 1);
  *p++ = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
  memcpy(p, name, name_len);
  p += name_len;
  adv_len = (uint8_t)(p - adv_buffer);
}

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static btstack_packet_callback_registration_t l2cap_event_callback_registration;

// ---- 受信 (RX characteristic への write) -----------------------------------
static void process_rx(const uint8_t *data, uint16_t len) {
  if (!cmd_authorized) {
    BOARD::diag_printf("[BLE_UART] RX dropped (unauthorized, %u byte)\n", len);
    return;
  }
  if (len == 0)
    return;
  frame_t f{};
  f.len = len > sizeof(frame_t::data) ? (uint16_t)sizeof(frame_t::data)
                                       : (uint16_t)len;
  memcpy(f.data, data, f.len);
  // ★LOSSLESS を付けていない (push は常に true) — 満杯なら黙って最古を上書き。
  //   producer (この関数) は決して待たない (stream.hpp の設計方針)。
  g_rx.hdl().push(f);
  BOARD::diag_printf("[BLE_UART] RX %u byte accepted (authorized)\n", len);
}

// ---- CI 強制 (旧実装のまま — 実測知見そのもの) -------------------------------
// 12 units = 15.00ms。Apple/macOS はこれ未満を要求すると数秒で強制切断する
// (macos-ble-min-conn-interval-15ms の実測知見)。
static constexpr uint16_t FORCED_CI = 12;
// ---- 生存監視 (旧 BLE_UART_DRIVER から移植, 2026-08-24) --------------------
// ★何を直しているか:
//   (a) 相手が黙って消えると con_handle が生きたままになり、**再アドバタイズ
//       しない**。ホスト側からは「device not found」に見え、OTA も GDB も
//       繋がらない。実測で何度も踏んでいる。
//   (b) それより悪い形として、CYW43 のコントローラごと無応答になることがある
//       (`[CYW43] Bus error` を伴う)。この場合ローカルの掃除では戻らず、
//       **チップの電源を入れ直す**しかない (HCI の OFF→ON では蘇生しないことを
//       旧実装で実測済み)。
// ★段を分ける理由: 掃除で戻るなら 2 秒止める必要は無いし、掃除で戻らない相手に
//   掃除を繰り返しても永久に戻らない。「掃除 → 応答があるか見る → 無ければ
//   電源」の順にすることで、**代償の大きい手を最後にだけ払う**。
static uint32_t hci_event_count = 0;      // 何か HCI/SM イベントが来た印
static uint64_t last_conn_activity_us = 0;
static bool wd_recovery_pending = false;  // 掃除したので応答を見張っている
static uint64_t wd_cleanup_us = 0;
static uint32_t wd_evt_snapshot = 0;
static bool restart_requested = false;    // 診断 CDC の 'R' から

// ★接続中の無音をどれだけ許すか。テレメトリが流れている限り CAN_SEND_NOW が
//   絶えず来るので、生きたリンクがここまで黙ることはない。GDB でブレーク
//   したままでもテレメトリは動き続けるので、そこで誤爆はしない。
static constexpr uint64_t CONN_IDLE_TIMEOUT_US = 20000000ull;
// 掃除への応答をどれだけ待つか。ここを過ぎたらコントローラ無応答とみなす。
static constexpr uint64_t WD_RECOVERY_TIMEOUT_US = 3000000ull;

static uint8_t ci_nego_stage = 0; // 0=未要求 / 1=(12,12)要求済み / 2=フォールバック済み

static void print_conn_interval(const char *tag) {
  uint32_t x100 = (uint32_t)conn_interval * 125u;
  BOARD::diag_printf("[BLE_UART] %s CI = %u units (%lu.%02lu ms)\n", tag,
                     conn_interval, (unsigned long)(x100 / 100),
                     (unsigned long)(x100 % 100));
}

static void request_fast_ci() {
  if (con_handle == HCI_CON_HANDLE_INVALID)
    return;
  ci_nego_stage = 1;
  gap_request_connection_parameter_update(con_handle, FORCED_CI, FORCED_CI, 0,
                                          400);
}

static void request_ci_fallback(const char *why) {
  if (con_handle == HCI_CON_HANDLE_INVALID || ci_nego_stage != 1)
    return;
  ci_nego_stage = 2;
  BOARD::diag_printf("[BLE_UART] CI fallback (%s): requesting (12,24)\n", why);
  gap_request_connection_parameter_update(con_handle, FORCED_CI, FORCED_CI * 2,
                                          0, 400);
}

// ---- ATT read/write callback ------------------------------------------------
static uint16_t att_read_callback(hci_con_handle_t, uint16_t, uint16_t, uint8_t *,
                                  uint16_t) {
  return 0; // TX は notify 専用、RX も値読み出しは無し
}

static int att_write_callback(hci_con_handle_t connection_handle,
                              uint16_t att_handle, uint16_t, uint16_t,
                              uint8_t *buffer, uint16_t buffer_size) {
  // ★書き込みも生存の証拠。OTA の転送中は notify がほとんど出ないので、
  //   ここを数えないと「9 秒黙っている」ように見えてしまう。
  last_conn_activity_us = BOARD::time_us();
  if (att_handle ==
      ATT_CHARACTERISTIC_6E400003_B5A3_F393_E0A9_E50E24DCCA9E_01_CLIENT_CONFIGURATION_HANDLE) {
    tx_notify_enabled =
        (little_endian_read_16(buffer, 0) ==
         GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
    con_handle = connection_handle;
    BOARD::diag_printf("[BLE_UART] notify %s\n",
                       tx_notify_enabled ? "enabled" : "disabled");
    if (tx_notify_enabled && !can_send_requested) {
      can_send_requested = true;
      att_server_request_can_send_now_event(con_handle);
    }
    return 0;
  }
  if (att_handle ==
      ATT_CHARACTERISTIC_6E400002_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE) {
    process_rx(buffer, buffer_size);
    return 0;
  }
  // ---- OTA ----
  if (att_handle ==
      ATT_CHARACTERISTIC_6E402002_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE) {
    // ★fail-closed。認可されていないリンクからファームは受け取らない。
    if (!cmd_authorized) {
      BOARD::diag_printf("[BLE_UART] ota write dropped (unauthorized, %u byte)\n",
                         buffer_size);
      return 0;
    }
    static uint32_t s_ota_pkt_count = 0;
    if (++s_ota_pkt_count >= 10) {
      s_ota_pkt_count = 0;
      led_toggle();
    }
    uint32_t offset = 0;
    while (offset < buffer_size) {
      frame_t f{};
      uint32_t n = buffer_size - offset;
      if (n > sizeof(f.data))
        n = sizeof(f.data);
      f.len = (uint16_t)n;
      memcpy(f.data, buffer + offset, n);
      g_ota_rx.hdl().push(f);
      offset += n;
    }
    return 0;
  }
  // ---- GDB リンク ----
  if (att_handle ==
      ATT_CHARACTERISTIC_6E401003_B5A3_F393_E0A9_E50E24DCCA9E_01_CLIENT_CONFIGURATION_HANDLE) {
    gdb_notify_enabled =
        (little_endian_read_16(buffer, 0) ==
         GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
    con_handle = connection_handle;
    update_gdb_connected();
    if (gdb_notify_enabled && !can_send_requested && tx_pending() != 0) {
      can_send_requested = true;
      att_server_request_can_send_now_event(con_handle);
    }
    BOARD::diag_printf("[BLE_UART] gdb notify %s (authorized=%d)\n",
                       gdb_notify_enabled ? "enabled" : "disabled",
                       cmd_authorized ? 1 : 0);
    return 0;
  }
  if (att_handle ==
      ATT_CHARACTERISTIC_6E401002_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE) {
    // ★fail-closed。認可前の RSP は捨てる (デバッガは繋がらないだけで、
    //   こちらの状態は一切変わらない)。
    if (!cmd_authorized) {
      BOARD::diag_printf("[BLE_UART] gdb write dropped (unauthorized, %u byte)\n",
                         buffer_size);
      return 0;
    }
    if (!gdb_saw_traffic) {
      gdb_saw_traffic = true; // ★本物のデバッガが喋った
      update_gdb_connected();
      BOARD::diag_printf("[BLE_UART] gdb traffic seen — stub is live\n");
    }
    if (!g_gdb_to_stub.valid())
      return 0;
    uint32_t offset = 0;
    while (offset < buffer_size) {
      link_chunk c{};
      uint32_t n = buffer_size - offset;
      if (n > sizeof(c.data))
        n = sizeof(c.data);
      c.len = (uint16_t)n;
      memcpy(c.data, buffer + offset, n);
      g_gdb_to_stub.push(c);
      offset += n;
    }
    return 0;
  }
  // ---- CH2 リンク (6E403001-...) ----
  if (att_handle ==
      ATT_CHARACTERISTIC_6E403003_B5A3_F393_E0A9_E50E24DCCA9E_01_CLIENT_CONFIGURATION_HANDLE) {
    ch2_notify_enabled =
        (little_endian_read_16(buffer, 0) ==
         GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
    con_handle = connection_handle;
    if (ch2_notify_enabled && !can_send_requested && tx_pending() != 0) {
      can_send_requested = true;
      att_server_request_can_send_now_event(con_handle);
    }
    BOARD::diag_printf("[BLE_UART] ch2 notify %s\n",
                       ch2_notify_enabled ? "enabled" : "disabled");
    return 0;
  }
  if (att_handle ==
      ATT_CHARACTERISTIC_6E403002_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE) {
    uint32_t offset = 0;
    while (offset < buffer_size) {
      frame_t f{};
      uint32_t n = buffer_size - offset;
      if (n > sizeof(f.data))
        n = sizeof(f.data);
      f.len = (uint16_t)n;
      memcpy(f.data, buffer + offset, n);
      g_ch2_rx.hdl().push(f);
      offset += n;
    }
    return 0;
  }
  return 0;
}

// ---- TX flush ---------------------------------------------------------------
// v1: 1 CAN_SEND_NOW で送れるだけ送る (優先度の作り分けは無い — 単一ストリーム)。
// 出口は 2 つ: ble_uart 自身の行 (g_tx) と、ハブから来る本線 (g_tx_in)。
// ★自分の行を先に出す — 接続状態などの内部メッセージは本数が少なく、
//   テレメトリのバルクに埋もれると読めなくなるため (旧実装が PRIO_SYS を
//   一番強くしていたのと同じ理由)。合流の方針をここに書けるのは、
//   ble_uart が**この 2 本の合流点**だから (D46 のハブが方針を持つ、の小型版)。
static bool notify_one(const frame_t &f) {
  return att_server_notify(
             con_handle,
             ATT_CHARACTERISTIC_6E400003_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE,
             f.data, f.len) == 0;
}

uint32_t tx_pending() {
  uint32_t n = g_tx.hdl().available();
  if (g_tx_in.valid())
    n += g_tx_in.available();
  if (g_gdb_from_stub.valid())
    n += g_gdb_from_stub.available();
  // ★手元に留めている 1 個も「残り」に数える。数えないと poll ループが
  //   CAN_SEND_NOW を要求しなくなり、**留めたまま二度と出ない**。
  if (g_gdb_held_valid)
    ++n;
  if (g_ch2_tx_in.valid())
    n += g_ch2_tx_in.available();
  if (g_ch2_held_valid)
    ++n;
  return n;
}

// GDB の返事を先に出す。★対話は 1 往復ごとに CI ぶん待つので、テレメトリの
//   バルクに後ろへ回されると往復がそのまま伸びる (RSP は往復回数が多い)。
// ★★★**取り出した以上、捨てない** (2026-08-25 に踏んだ)。
//   以前はここで「pop してから notify、失敗したら break」としていた。
//   `att_server_notify` はクレジットが無いと失敗するので、**その 1 個は
//   リングから消えたまま二度と送られない**。コメントには「次の
//   CAN_SEND_NOW で続きを出す」と書いてあったが、続きも何も、
//   取り出した本人が落としていた。
//   ★これが出るのは**長い返事のときだけ**。短い返事はクレジットに収まるので
//     露見しない。D53 で target.xml (830B = 14 チャンク) を返すように
//     なった瞬間に表面化し、GDB からは `Ignoring packet error` と、
//     再送の繰り返しによる **attach 152 秒**として見えていた。
//   ★スタブ側 (gdb_stub.cpp の flush_out) にも同じ形の穴があって、そちらは
//     先に直した。**送り手と受け手の両方**を直さないと落ちなくならない。
//   出せなかった 1 個は手元に置いて、次の機会に**それから**出す。
static void flush_ch2() {
  if (con_handle == HCI_CON_HANDLE_INVALID || !ch2_notify_enabled)
    return;
  if (!g_ch2_tx_in.valid())
    return;
  const uint16_t handle =
      ATT_CHARACTERISTIC_6E403003_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE;
  if (g_ch2_held_valid) {
    if (att_server_notify(con_handle, handle, g_ch2_held.data,
                          g_ch2_held.len) != 0)
      return;
    g_ch2_held_valid = false;
  }
  frame_t f{};
  while (g_ch2_tx_in.available() != 0) {
    if (!g_ch2_tx_in.pop(&f))
      return;
    if (att_server_notify(con_handle, handle, f.data, f.len) != 0) {
      g_ch2_held = f;
      g_ch2_held_valid = true;
      return;
    }
  }
}

static void flush_gdb() {
  if (con_handle == HCI_CON_HANDLE_INVALID || !gdb_notify_enabled)
    return;
  if (!g_gdb_from_stub.valid())
    return;
  const uint16_t handle =
      ATT_CHARACTERISTIC_6E401003_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE;
  // 前回出せなかったものが先。順番を崩すと RSP は壊れる。
  if (g_gdb_held_valid) {
    if (att_server_notify(con_handle, handle, g_gdb_held.data,
                          g_gdb_held.len) != 0)
      return; // まだクレジットが無い。次の CAN_SEND_NOW で。
    g_gdb_held_valid = false;
  }
  link_chunk c{};
  while (g_gdb_from_stub.available() != 0) {
    if (!g_gdb_from_stub.pop(&c))
      break;
    if (att_server_notify(con_handle, handle, c.data, c.len) != 0) {
      g_gdb_held = c; // ★捨てない
      g_gdb_held_valid = true;
      return;
    }
  }
}

static void flush_tx() {
  flush_gdb();
  flush_ch2();
  if (con_handle == HCI_CON_HANDLE_INVALID || !tx_notify_enabled)
    return;
  frame_t f{};
  // 1) 自分の行
  while (g_tx.hdl().available() != 0) {
    if (!g_tx.hdl().pop(&f))
      break;
    // ★クレジット切れ。v1 は pop 済みなのでこのフレームは失われる (旧実装は
    //   peek→成功後 drop で再送を保証していた)。
    if (!notify_one(f))
      goto done;
  }
  // 2) ハブから来た本線
  if (g_tx_in.valid()) {
    uint32_t lost = 0;
    while (g_tx_in.available() != 0) {
      if (!g_tx_in.pop(&f, &lost))
        break;
      if (!notify_one(f))
        goto done;
    }
  }
done:
  if (tx_pending() != 0) {
    can_send_requested = true;
    att_server_request_can_send_now_event(con_handle);
  }
}

// 広告を出してよい時間か。★止めるのは DoS クールダウン中だけ。
//   「ボンド済みなら広告を完全に止める」案は採らなかった (下の poll ループの
//   コメント参照)。
static bool advertising_allowed(uint64_t now_us) {
  return g_pair_block_until_us == 0 || now_us >= g_pair_block_until_us;
}

// ---- HCI / ATT イベント (旧実装から診断・ウォッチドッグ・PHY 系を落とした版) ----
static void packet_handler(uint8_t packet_type, uint16_t, uint8_t *packet,
                           uint16_t) {
  // ★「何か来た」印。ウォッチドッグはこの数字が動くかどうかだけを見る。
  ++hci_event_count;
  last_conn_activity_us = BOARD::time_us();
  if (packet_type != HCI_EVENT_PACKET)
    return;
  uint8_t event = hci_event_packet_get_type(packet);
  switch (event) {
  case BTSTACK_EVENT_STATE:
    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
      BOARD::diag_printf("[BLE_UART] HCI ready, advertising start\n");
      gap_advertisements_enable(1);
    }
    break;
  case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE: {
    uint16_t result =
        l2cap_event_connection_parameter_update_response_get_result(packet);
    if (result != 0)
      request_ci_fallback("rejected");
    break;
  }
  case HCI_EVENT_LE_META:
    switch (hci_event_le_meta_get_subevent_code(packet)) {
    case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
      uint8_t st = hci_subevent_le_connection_complete_get_status(packet);
      if (st != ERROR_CODE_SUCCESS) {
        BOARD::diag_printf(
            "[BLE_UART] connection failed (status=0x%02x), re-advertising\n",
            st);
        if (advertising_allowed(BOARD::time_us()))
          gap_advertisements_enable(1);
        break;
      }
      hci_con_handle_t h =
          hci_subevent_le_connection_complete_get_connection_handle(packet);
      if (h == con_handle)
        break; // 二重配送ガード
      con_handle = h;
      conn_started_us = BOARD::time_us();
      conn_interval =
          hci_subevent_le_connection_complete_get_conn_interval(packet);
      tx_notify_enabled = false;
      can_send_requested = false;
      cmd_authorized = false;
      update_gdb_connected();
      ci_nego_stage = 0;
      BOARD::diag_printf("[BLE_UART] connected, handle=0x%04x (requesting pairing)\n",
                         h);
      sm_request_pairing(con_handle);
      break;
    }
    case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
      conn_interval =
          hci_subevent_le_connection_update_complete_get_conn_interval(packet);
      print_conn_interval("updated");
      if (conn_interval != FORCED_CI)
        request_ci_fallback("updated CI != 12");
      break;
    default:
      break;
    }
    break;
  case HCI_EVENT_DISCONNECTION_COMPLETE:
    BOARD::diag_printf("[BLE_UART] disconnected (reason=0x%02x), re-advertising\n",
                       hci_event_disconnection_complete_get_reason(packet));
    con_handle = HCI_CON_HANDLE_INVALID;
    tx_notify_enabled = false;
    can_send_requested = false;
    cmd_authorized = false;
    gdb_notify_enabled = false; // 切断で GDB も落ちる (次の接続で張り直す)
    gdb_saw_traffic = false;
    g_gdb_held_valid = false;   // 前の接続の断片を次へ持ち越さない
    update_gdb_connected();
    ci_nego_stage = 0;
    nc_pending_handle = HCI_CON_HANDLE_INVALID;
    if (advertising_allowed(BOARD::time_us()))
      gap_advertisements_enable(1);
    break;
  case ATT_EVENT_CAN_SEND_NOW:
    can_send_requested = false;
    flush_tx();
    break;
  default:
    break;
  }
}

// ---- SM (ペアリング/暗号化) イベント ----------------------------------------
static void sm_packet_handler(uint8_t packet_type, uint16_t, uint8_t *packet,
                              uint16_t) {
  ++hci_event_count;
  last_conn_activity_us = BOARD::time_us();
  if (packet_type != HCI_EVENT_PACKET)
    return;
  switch (hci_event_packet_get_type(packet)) {
  case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
    const hci_con_handle_t h =
        sm_event_numeric_comparison_request_get_handle(packet);
    // ★施錠中はここまで来ないはず (Pairing Request の時点で
    //   sm_set_accepted_stk_generation_methods(0) が弾く) が、**二重の網**として
    //   ここでも拒む。理由: 拒否の一次防壁がグローバル変数 1 個に乗っているので、
    //   将来 apply_security_mode() の呼び忘れが 1 箇所でも入ると穴になる。
    //   ここは「今まさに人に承認を求めようとしている」地点なので、施錠の意思が
    //   最後に効く場所として自然。
    if (pairing_locked_now(BOARD::time_us())) {
      sm_bonding_decline(h);
      ++g_pair_strikes;
      BOARD::diag_printf(
          "[BLE_UART] pairing DECLINED (locked, bonds=%lu, strike %lu)\n",
          (unsigned long)g_bonded_count, (unsigned long)g_pair_strikes);
      break;
    }
    // ★事故防止のため必須: 番号を CDC へ出し、オペレータの y/n 確認を
    //   ble_uart_poll 側で待つ (自動 confirm にしない = MITM 防御が成立する)。
    // ★★承認の口は CDC だけではない。飛行前に USB は繋がないので、
    //   有線の XIAO (UART0) からも同じ答えを入れられるように、番号と
    //   「待っている」ことを pairing_state として外へ出す (GET_PAIRING_STATE)。
    //   ここで UART へ直接書かないのは、UART0 の持ち主が shizuku_shell 側で、
    //   二人で書くと行が混ざるため (読み手も一人に保つのと同じ理由)。
    nc_pending_handle = h;
    g_nc_passkey =
        (uint32_t)sm_event_numeric_comparison_request_get_passkey(packet);
    ++g_nc_generation;
    BOARD::diag_printf("[BLE_UART] === NUMERIC COMPARISON ===\n");
    BOARD::diag_printf("[BLE_UART] device number : %06lu\n",
                       (unsigned long)g_nc_passkey);
    BOARD::diag_printf("[BLE_UART] 一致していれば CDC で 'y' / XIAO で 'nc y'、"
                       "違えば 'n' / 'nc n'\n");
    break;
  }
  case SM_EVENT_JUST_WORKS_REQUEST: {
    // ★Just Works は MITM 防御が無い。btstack は応答しなければ黙って先へ進む
    //   仕様 (sm.c: sm_user_response が IDLE のままなら自動継続) なので、
    //   **拒みたいならここで明示的に decline しなければならない**。
    const hci_con_handle_t h = sm_event_just_works_request_get_handle(packet);
    if (pairing_locked_now(BOARD::time_us())) {
      sm_bonding_decline(h);
      ++g_pair_strikes;
      BOARD::diag_printf("[BLE_UART] just-works DECLINED (locked, strike %lu)\n",
                         (unsigned long)g_pair_strikes);
    } else if (!g_auto_pair) {
      // ★NC 必須モードなのに Just Works に落ちた = 相手が MITM 防御を要求して
      //   いない (格下げ)。要求した保護が得られていない以上、通してはいけない。
      sm_bonding_decline(h);
      ++g_pair_strikes;
      BOARD::diag_printf(
          "[BLE_UART] just-works DECLINED (SECURE モードでの格下げ拒否)\n");
    }
    // AUTO モードでは**何もしない** = btstack がそのまま自動継続する。
    // ここで sm_just_works_confirm() を呼んでも等価だが、実機で通っている
    // 経路を触らない (テスト用モードのために本番経路を動かす価値が無い)。
    break;
  }
  case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
    BOARD::diag_printf(
        "[BLE_UART] passkey (enter on phone) = %06lu\n",
        (unsigned long)sm_event_passkey_display_number_get_passkey(packet));
    break;
  case SM_EVENT_PAIRING_COMPLETE:
    nc_pending_handle = HCI_CON_HANDLE_INVALID;
    if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
      cmd_authorized = true;
      update_gdb_connected();
      // ★一回券は**使ったら失効**させる。時間切れを待つと「解錠したまま
      //   飛ばす」窓が残り、施錠の意味が薄れる。
      g_pair_allow_until_us = 0;
      g_pair_strikes = 0;
      g_pair_mode_dirty = true; // 新しいボンドを数え直して施錠を掛け直す
      BOARD::diag_printf("[BLE_UART] pairing complete -> authorized (再施錠)\n");
      print_conn_interval("paired");
      request_fast_ci();
    } else {
      cmd_authorized = false;
      update_gdb_connected();
      ++g_pair_strikes;
      BOARD::diag_printf("[BLE_UART] pairing failed (status 0x%02x, strike %lu)\n",
                         sm_event_pairing_complete_get_status(packet),
                         (unsigned long)g_pair_strikes);
    }
    break;
  case SM_EVENT_REENCRYPTION_COMPLETE:
    if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
      cmd_authorized = true;
      update_gdb_connected();
      // ★正規の相手が戻ってきた = それまでの失敗は攻撃かノイズかに関わらず
      //   「今は正常」なので数え直す。ここで消さないと、たまたま失敗が
      //   積もっていただけで正規運用中に広告が止まる。
      g_pair_strikes = 0;
      BOARD::diag_printf("[BLE_UART] reencryption complete -> authorized\n");
      print_conn_interval("paired");
      request_fast_ci();
    } else {
      cmd_authorized = false;
      update_gdb_connected();
      BOARD::diag_printf("[BLE_UART] reencryption failed -> stays locked\n");
    }
    break;
  default:
    break;
  }
}

// ---- エクスポートするメソッド ------------------------------------------------
// a0 = ハブ (flight_controller) の TX ストリーム番号。番号を控えるだけで、
// consumer 席に座る (STREAM_BIND) のは poll スレッドの冒頭 — 席は**発行元の
// オブジェクト**から導出されるので、実際に pop する側のスレッドで座らないと
// 意味がないため。
uintptr_t method_set_tx_stream(uintptr_t argument, uintptr_t, uintptr_t,
                               uintptr_t) {
  g_tx_in_id = argument;
  return 1;
}

// a0 = (stub へ送る番号 << 16) | (stub から受ける番号)。CALL_METHOD の引数枠が
// 1 つなので詰めて渡す (旧 register_tx_stream と同じ手)。
uintptr_t method_get_ch2_rx_stream(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_ch2_rx_stream_id;
}

uintptr_t method_set_ch2_tx_stream(uintptr_t argument, uintptr_t, uintptr_t,
                                   uintptr_t) {
  g_ch2_tx_in_id = argument;
  return 1;
}

uintptr_t method_get_ota_stream(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_ota_stream_id;
}

// ★旗を立てるだけ。btstack を触るのは poll ループの担当 (ble_uart.hpp の
//   REQUEST_DISCONNECT のコメント)。
volatile bool g_drop_link_requested = false;
uintptr_t method_request_disconnect(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_drop_link_requested = true;
  return 0;
}

uintptr_t method_set_gdb_streams(uintptr_t argument, uintptr_t, uintptr_t,
                                 uintptr_t) {
  g_gdb_to_stub_id = argument >> 16;
  g_gdb_from_stub_id = argument & 0xFFFFu;
  return 1;
}

uintptr_t method_get_rx_stream(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_rx_stream_id;
}

// ---- ペアリング施錠の外向き口 (呼ぶのは shizuku_shell = 別スレッド) ---------
// ★どれも **btstack を一切触らない**。触るのは poll ループの担当
//   ([[no-btstack-from-caller-thread]]: 呼び出し元スレッドから btstack に
//   入ると CYW43 の SPI バスごと固まる)。ここでは旗を立てるだけ。
uintptr_t method_get_pairing_state(uintptr_t out, uintptr_t, uintptr_t,
                                   uintptr_t) {
  if (out == 0)
    return 0;
  auto *s = (shizuku::objects::ble_uart::pairing_state *)out;
  const uint64_t now = BOARD::time_us();
  *s = {};
  s->nc_generation = g_nc_generation;
  s->nc_passkey = g_nc_passkey;
  s->strikes = g_pair_strikes;
  s->nc_pending = nc_pending_handle != HCI_CON_HANDLE_INVALID ? 1 : 0;
  s->locked = pairing_locked_now(now) ? 1 : 0;
  // ★件数は写しを返す。le_device_db_count() を直に呼ぶと btstack の内部を
  //   別スレッドから読むことになる (今の実装は単なる static int だが、
  //   「btstack を触ってよいのは poll ループだけ」の線を実装詳細で崩さない)。
  s->bonded = (uint8_t)g_bonded_count;
  s->connected = con_handle != HCI_CON_HANDLE_INVALID ? 1 : 0;
  s->authorized = cmd_authorized ? 1 : 0;
  if (g_pair_allow_until_us != 0 && now < g_pair_allow_until_us)
    s->allow_seconds_left = (uint32_t)((g_pair_allow_until_us - now) / 1000000u);
  if (g_pair_block_until_us != 0 && now < g_pair_block_until_us)
    s->block_seconds_left = (uint32_t)((g_pair_block_until_us - now) / 1000000u);
  return 1;
}

uintptr_t method_pairing_answer(uintptr_t confirm, uintptr_t, uintptr_t,
                                uintptr_t) {
  if (nc_pending_handle == HCI_CON_HANDLE_INVALID)
    return 0; // 訊かれていないのに答えない (取り違えを防ぐ)
  g_pair_answer_requested = confirm != 0 ? 1 : 2;
  return 1;
}

uintptr_t method_set_pairing_lock(uintptr_t lock, uintptr_t, uintptr_t,
                                  uintptr_t) {
  if (lock != 0) {
    g_pair_allow_until_us = 0; // 出した一回券を取り消す = 即施錠
  } else {
    // ★「解錠モード」にはしない。一回券 (時間で失効・ペアリング成立でも失効)
    //   にすることで、「戻し忘れたまま飛ばす」を構造的に潰す。
    g_pair_allow_until_us = BOARD::time_us() + PAIRING_ALLOW_WINDOW_US;
  }
  g_pair_mode_dirty = true;
  return 1;
}

uintptr_t method_forget_bonds(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_forget_bonds_requested = true;
  return 1;
}

// ---- BT スタック起動 (旧実装の bt_stack_bringup から診断/Stage1/2 を落とした版) --
static void bt_stack_bringup() {
  // ★cyw43_arch_init() はここで呼ばない。pico2_w では LED オブジェクト
  //   (peripherals.cpp の led_main) が既に core0 から呼んでチップとファームを
  //   立ち上げ済み。二重に呼ぶと (PIO/IRQ の再取得・チップへのファーム再送信)
  //   ハングする実測を踏んだ ("各オブジェクトのprintfを確認せよ" の指摘で
  //   コードレビューにより発見 — [XNO BOOT] peripherals registered までは
  //   出るが ble_uart registered が出ない、という無音の切り分けと符合する)。
  //   register_ble_uart() は register_peripherals() の後に呼ぶこと (この
  //   前提が崩れると cyw43 未初期化のまま l2cap_init 等に入ってしまう)。
  l2cap_init();

  // ★bonding の永続化はここでは**何もしない**。pico-sdk の
  //   `btstack_cyw43_init()` (cyw43_arch_init から呼ばれる) が既に
  //   `setup_tlv()` で `btstack_tlv_flash_bank_init_instance` +
  //   `btstack_tlv_set_instance` + `le_device_db_tlv_configure` まで
  //   済ませている (SDK の btstack_cyw43.c:28-47)。
  //   ★★ここで**同じことをもう一度やると板が起動しなくなる** (2026-08-24 実測)。
  //   同じ物理バンクを 2 つの `btstack_tlv_flash_bank_t` が別々に管理する形に
  //   なり、バンクの整合が崩れる → 次回以降の
  //   `btstack_tlv_flash_bank_init_instance` が「消してやり直す」判断をする →
  //   その erase が `flash_safe_execute` を呼ぶ → **cyw43_arch_init の中なので
  //   core1 はまだ起きておらず** `multicore_lockout_victim_is_initialized(1)`
  //   が false → `assert(false)` (pico_flash/flash.c:190) で停止する。
  //   flash に残るので電源再投入でもアプリの焼き直しでも直らず、
  //   `picotool erase --all` でしか復旧できない状態になる。
  sm_init();

  // ★施錠の判断材料 (保存済みボンドの件数) をここで一度読む。
  //   TLV の走査は SDK の `btstack_cyw43_init()` が
  //   `le_device_db_tlv_configure()` の中で済ませており (le_device_db_tlv.c の
  //   `le_device_db_tlv_scan`)、`sm_init()` 側の `le_device_db_init()` は
  //   TLV 実装では no-op。つまりこの時点で件数は正しい。
  //   ★ここで読まないと、poll スレッドが最初の 1 秒を回すまで**解錠状態**の
  //     まま HCI が上がってしまい、その窓で新規ペアリングが通る。
  g_bonded_count = (uint32_t)le_device_db_count();
  // 既定 (g_auto_pair=false) = NC 必須。'A'/'S' コマンドで実行時に切替可能
  // (apply_security_mode 参照)。
  apply_security_mode();

  att_server_init(profile_data, att_read_callback, att_write_callback);

  hci_event_callback_registration.callback = &packet_handler;
  hci_add_event_handler(&hci_event_callback_registration);
  att_server_register_packet_handler(packet_handler);
  sm_event_callback_registration.callback = &sm_packet_handler;
  sm_add_event_handler(&sm_event_callback_registration);
  l2cap_event_callback_registration.callback = &packet_handler;
  l2cap_add_event_handler(&l2cap_event_callback_registration);

  build_adv_data();
  bd_addr_t null_addr;
  memset(null_addr, 0, sizeof(null_addr));
  gap_advertisements_set_params(0x0030, 0x0060, 0 /*ADV_IND*/, 0, null_addr,
                                0x07, 0x00);
  gap_advertisements_set_data(adv_len, adv_buffer);

  hci_power_control(HCI_POWER_ON);
}

// ---- BT スタック解体 → チップ電源断 ----------------------------------------
// ★上位層を**明示的に** deinit する。btstack の init 群は再初期化ガード付きで、
//   deinit しないと次の init が黙って no-op になる (= 作り直したつもりで
//   古い状態のまま走る、という一番たちの悪い形)。
// ★cyw43_arch_deinit() は内部で hci_power_control(OFF) + hci_close + run loop の
//   解体までやり、チップの電源も落とす。次の cyw43_arch_init でファームが
//   再ロードされる = コアの完全リセット。
static void bt_stack_teardown() {
  att_server_deinit();
  sm_deinit();
  l2cap_deinit();
  cyw43_arch_deinit();
}

// コントローラ完全無応答からの最終手段。★およそ 2 秒ブロックする。
// ★★**この間 yield しない**のが肝。poll スレッドは budget-0 (バトン) なので、
//   譲らなければ他のスレッドは走らない = 解体中の cyw43 を誰も触らない。
//   LED (peripherals.cpp) も同じ core0 に居て cyw43 越しに光るので、ここで
//   譲ると「チップが居ない瞬間に LED が書きに来る」窓ができる。
//   旧実装はロックでこれを塞いでいたが、こちらはロックが無いぶん
//   **譲らないことで塞ぐ**。
static void bt_full_chip_restart() {
  BOARD::diag_printf("[BLE_UART] full CYW43 chip reset...\n");
  con_handle = HCI_CON_HANDLE_INVALID;
  tx_notify_enabled = false;
  can_send_requested = false;
  cmd_authorized = false;
  gdb_notify_enabled = false;
  gdb_saw_traffic = false;
  update_gdb_connected();
  ci_nego_stage = 0;
  nc_pending_handle = HCI_CON_HANDLE_INVALID;

  bt_stack_teardown();
  busy_wait_us(100000); // 電源断を落ち着かせる。★sleep ではない (譲らない)
  if (cyw43_arch_init() != 0) {
    BOARD::diag_printf("[BLE_UART] cyw43_arch_init FAILED after reset\n");
    return;
  }
  bt_stack_bringup();
  last_conn_activity_us = BOARD::time_us();
  BOARD::diag_printf("[BLE_UART] chip reset done, waiting for HCI ready\n");
}

// ---- ポーリングスレッド (SPAWN で起こす。budget-0 = 主ループに戻す/戻される
//   バトン渡し — cyw43/btstack 内部が preemption-safe でないため) ------------
uintptr_t poll_loop(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★席に座るのはここ (このスレッド) — STREAM_BIND は発行元のオブジェクトから
  //   席を導出するので、実際に読み書きする側で座る。RX は producer (btstack の
  //   write コールバックがここのスレッドで走る)、ハブからの本線は consumer。
  api(shizuku::object_api::STREAM_BIND, g_rx_stream_id,
      (uintptr_t)shizuku::stream::role::PRODUCER);
  if (g_ch2_tx_in_id != NO_STREAM) {
    const auto opened = api(shizuku::object_api::STREAM_OPEN, g_ch2_tx_in_id);
    if (opened.error == 0 && opened.value != 0) {
      g_ch2_tx_in = shizuku::stream::handle<frame_t>(
          (shizuku::stream::descriptor *)opened.value);
      api(shizuku::object_api::STREAM_BIND, g_ch2_tx_in_id,
          (uintptr_t)shizuku::stream::role::CONSUMER);
    }
  }

  if (g_tx_in_id != NO_STREAM) {
    const auto opened = api(shizuku::object_api::STREAM_OPEN, g_tx_in_id);
    if (opened.error == 0 && opened.value != 0) {
      g_tx_in = shizuku::stream::handle<frame_t>(
          (shizuku::stream::descriptor *)opened.value);
      api(shizuku::object_api::STREAM_BIND, g_tx_in_id,
          (uintptr_t)shizuku::stream::role::CONSUMER);
    } else {
      BOARD::diag_printf("[BLE_UART] TX stream %lu could not be opened (%lu)\n",
                         (unsigned long)g_tx_in_id, (unsigned long)opened.error);
    }
  } else {
    BOARD::diag_printf("[BLE_UART] no TX stream wired — 自分の行だけ送る\n");
  }

  // ---- OTA 受信の席 (自分が producer) ----
  api(shizuku::object_api::STREAM_BIND, g_ota_stream_id,
      (uintptr_t)shizuku::stream::role::PRODUCER);

  // ---- GDB リンクの席 ----
  if (g_gdb_to_stub_id != NO_STREAM &&
      g_gdb_from_stub_id != NO_STREAM) {
    const auto to_stub = api(shizuku::object_api::STREAM_OPEN, g_gdb_to_stub_id);
    const auto from_stub =
        api(shizuku::object_api::STREAM_OPEN, g_gdb_from_stub_id);
    if (to_stub.error == 0 && to_stub.value != 0 && from_stub.error == 0 &&
        from_stub.value != 0) {
      g_gdb_to_stub = shizuku::stream::handle<link_chunk>(
          (shizuku::stream::descriptor *)to_stub.value);
      g_gdb_from_stub = shizuku::stream::handle<link_chunk>(
          (shizuku::stream::descriptor *)from_stub.value);
      api(shizuku::object_api::STREAM_BIND, g_gdb_to_stub_id,
          (uintptr_t)shizuku::stream::role::PRODUCER);
      api(shizuku::object_api::STREAM_BIND, g_gdb_from_stub_id,
          (uintptr_t)shizuku::stream::role::CONSUMER);
      BOARD::diag_printf("[BLE_UART] gdb link on streams %lu/%lu\n",
                         (unsigned long)g_gdb_to_stub_id,
                         (unsigned long)g_gdb_from_stub_id);
    } else {
      BOARD::diag_printf("[BLE_UART] gdb link streams could not be opened\n");
    }
  }

  // ★★このスレッドは**止められては困る** (D56)。GDB の `monitor target` で
  //   これを選べてしまうと、止めた瞬間に RSP を運ぶ者が居なくなり、
  //   デバッガ自身が黙る = 止めた本人が復旧できない。無線だと電源再投入まで
  //   戻らないので、「やってみて壊れる」に任せてよい種類の操作ではない。
  //   ★本来は System Object が「誰が誰を止めてよいか」を持つべきで、
  //     ここで申告するのは資源の階層がまだ無いための繋ぎ。
  shizuku::objects::gdb_link_protect_thread(
      shizuku::kernel_instance.current_thread_id());

  uint64_t next_adv_ensure_us = 0;
  uint64_t next_bond_check_us = 0;
  uint64_t forget_deadline_us = 0;
  last_conn_activity_us = BOARD::time_us();
  while (true) {
    cyw43_arch_poll();
#if defined(CYW43_WL_GPIO_LED_PIN)
    led_sync_hw();
#endif

    // ★頼まれていたら切る。ここでやるのは、btstack を突いてよいのが
    //   このループだけだから (ble_uart.hpp の REQUEST_DISCONNECT 参照)。
    if (g_drop_link_requested) {
      g_drop_link_requested = false;
      if (con_handle != HCI_CON_HANDLE_INVALID) {
        BOARD::diag_printf("[BLE_UART] dropping the link on request\n");
        gap_disconnect(con_handle);
      }
    }

    // USB CDC (診断チャネル) からの 1 文字コマンドを覗く (非ブロッキング)。
    // NC 確認待ちなら y/n、そうでなければペアリング方式の切替コマンド。
    {
      int c = getchar_timeout_us(0);
      if (nc_pending_handle != HCI_CON_HANDLE_INVALID) {
        if (c == 'y' || c == 'Y') {
          BOARD::diag_printf("[BLE_UART] numeric comparison confirmed\n");
          sm_numeric_comparison_confirm(nc_pending_handle);
          nc_pending_handle = HCI_CON_HANDLE_INVALID;
        } else if (c == 'n' || c == 'N') {
          BOARD::diag_printf("[BLE_UART] numeric comparison declined\n");
          sm_bonding_decline(nc_pending_handle);
          // ★拒否は「その相手は自分ではなかった」の表明。UART 経由の拒否
          //   (上の (a)) と同じく失敗として数える — 数え方が経路で違うと、
          //   DoS 判定が「どちらの口から拒んだか」に依存してしまう。
          ++g_pair_strikes;
          nc_pending_handle = HCI_CON_HANDLE_INVALID;
        }
      } else if (c == 'A') {
        g_auto_pair = true;
        apply_security_mode();
        BOARD::diag_printf(
            "[BLE_UART] pairing mode -> AUTO (Just Works, no MITM). "
            "次の接続から有効\n");
      } else if (c == 'R') {
        // ★手で起こせるようにしておく。壊れてからしか通らない経路を
        //   壊れる前に一度通しておかないと、いざという時に動く保証が無い。
        BOARD::diag_printf("[BLE_UART] chip reset requested from the console\n");
        restart_requested = true;
      } else if (c == 'S') {
        g_auto_pair = false;
        apply_security_mode();
        BOARD::diag_printf(
            "[BLE_UART] pairing mode -> SECURE (NC required). 次の接続から有効\n");
      }
    }

    // ---- ペアリング施錠まわりの世話 ------------------------------------
    // ★btstack (sm_*/gap_*/le_device_db_*) を叩いてよいのはこのループだけ。
    //   外 (シェル) から来るのは旗だけで、実際の操作はここで代行する
    //   ([[no-btstack-from-caller-thread]])。
    {
      const uint64_t now = BOARD::time_us();

      // (a) 外部からの NC 承認/拒否。CDC の 'y'/'n' と**同じ答え**を、
      //     有線の XIAO (UART0 → shizuku_shell → CALL_METHOD) からも入れられる
      //     ようにするための口。飛行前に USB を繋ぐ運用が無いので、承認経路が
      //     CDC しか無いと NC が実質使えなかった。
      if (g_pair_answer_requested != 0) {
        const uint8_t answer = g_pair_answer_requested;
        g_pair_answer_requested = 0;
        if (nc_pending_handle != HCI_CON_HANDLE_INVALID) {
          if (answer == 1) {
            BOARD::diag_printf("[BLE_UART] numeric comparison confirmed (wire)\n");
            sm_numeric_comparison_confirm(nc_pending_handle);
          } else {
            BOARD::diag_printf("[BLE_UART] numeric comparison declined (wire)\n");
            sm_bonding_decline(nc_pending_handle);
            ++g_pair_strikes;
          }
          nc_pending_handle = HCI_CON_HANDLE_INVALID;
        }
      }

      // (b) 鍵を全部消す (施錠から締め出されたときの唯一の逃げ道)。
      //     ★★消す前にリンクを落とす。鍵の削除は TLV 経由で flash を書き、
      //       その中の `flash_safe_execute` が**全コアの割り込みを止める**。
      //       BLE が流れている最中にやると CYW43 の共有バスの index がずれて
      //       板ごと無応答になる ([[long-irq-off-wedges-cyw43]]、OTA で実測)。
      //     ★切れるのを待つが、待ち続けはしない — 相手が切断に応じない場合に
      //       「逃げ道が使えない」ほうが困る。3 秒で見切って消す。
      if (g_forget_bonds_requested) {
        if (con_handle != HCI_CON_HANDLE_INVALID) {
          if (forget_deadline_us == 0) {
            forget_deadline_us = now + 3000000ull;
            BOARD::diag_printf("[BLE_UART] forget bonds: dropping the link first\n");
            gap_disconnect(con_handle);
          }
          if (now < forget_deadline_us) {
            api(shizuku::object_api::SLEEP_US, 1000);
            continue; // まだ切れていない。次周でもう一度見る
          }
          BOARD::diag_printf("[BLE_UART] forget bonds: link did not drop, 続行\n");
        }
        g_forget_bonds_requested = false;
        forget_deadline_us = 0;
        uint32_t removed = 0;
        for (int i = 0; i < le_device_db_max_count(); ++i) {
          int type = (int)BD_ADDR_TYPE_UNKNOWN;
          bd_addr_t addr;
          le_device_db_info(i, &type, addr, nullptr);
          if (type == (int)BD_ADDR_TYPE_UNKNOWN)
            continue; // 空き枠
          gap_delete_bonding((bd_addr_type_t)type, addr);
          ++removed;
        }
        g_pair_allow_until_us = 0;
        g_pair_strikes = 0;
        g_pair_block_until_us = 0;
        g_pair_mode_dirty = true;
        BOARD::diag_printf(
            "[BLE_UART] forgot %lu bond(s) — 次の接続は新規ペアリングになる\n",
            (unsigned long)removed);
      }

      // (c) ボンド件数の追跡と施錠の再適用。
      //     ★毎周ではなく 1 秒ごと。件数は人の操作でしか動かないのに、
      //       le_device_db_count() を 1kHz で呼ぶ意味が無い。
      if (g_pair_mode_dirty || now >= next_bond_check_us) {
        g_pair_mode_dirty = false;
        next_bond_check_us = now + 1000000ull;
        if (g_pair_allow_until_us != 0 && now >= g_pair_allow_until_us) {
          g_pair_allow_until_us = 0;
          BOARD::diag_printf("[BLE_UART] pairing allow window expired -> locked\n");
        }
        const uint32_t counted = (uint32_t)le_device_db_count();
        const bool was_locked = pairing_locked_now(now);
        g_bonded_count = counted;
        apply_security_mode();
        if (was_locked != pairing_locked_now(now))
          BOARD::diag_printf("[BLE_UART] pairing %s (bonds=%lu)\n",
                             pairing_locked_now(now) ? "LOCKED" : "UNLOCKED",
                             (unsigned long)counted);
      }

      // (d) ペアリング DoS への保険 — 失敗が続いたら**一定時間広告を止める**。
      //   ★捨てた案と理由:
      //     - 「ボンド済みなら広告を常時止める」: 攻撃者は繋げなくなるが、
      //       **こちらも繋げなくなる**。BLE OTA が唯一の遠隔書き込み手段
      //       なので、見えない板は文鎮化と同義。却下。
      //     - directed advertising: 相手の identity address が要る。macOS は
      //       resolvable private address を使うので、解決リスト無しでは
      //       当たらない。かつ high-duty directed は 1.28 秒でしか出ず、
      //       復旧用の新しいホストからは永久に見えなくなる。却下。
      //     - whitelist (filter policy 0x03): 筋は良いが、RPA を弾かないため
      //       コントローラ側の resolving list 設定が要り、外すと**自分が
      //       締め出される**種類の設定。実機で確かめられない今は入れない。
      //   ★残したのは「速度を落とす」だけの手。完全には防げない (広告が
      //     見えれば誰でも接続は試せる) ことは受け入れて、1 分間の沈黙で
      //     連打の採算を壊す。自動で明けるので締め出しにはならない。
      if (g_pair_strikes >= PAIR_STRIKE_LIMIT && advertising_allowed(now)) {
        g_pair_strikes = 0;
        g_pair_block_until_us = now + PAIR_BLOCK_US;
        gap_advertisements_enable(0);
        if (con_handle != HCI_CON_HANDLE_INVALID)
          gap_disconnect(con_handle);
        BOARD::diag_printf(
            "[BLE_UART] too many failed pairings — advertising off for %lus\n",
            (unsigned long)(PAIR_BLOCK_US / 1000000ull));
      }
      if (g_pair_block_until_us != 0 && now >= g_pair_block_until_us) {
        g_pair_block_until_us = 0;
        next_adv_ensure_us = 0; // ③ に即座に張り直させる
        BOARD::diag_printf("[BLE_UART] pairing cooldown over — advertising again\n");
      }

      // (e) 認可されないまま居座るリンクを畳む。
      //   ★BLE の接続は **1 本しか持てない**ので、繋いで黙っているだけで
      //     正規の母艦を締め出せる。これがいちばん安い DoS で、しかも
      //     ①の生存ウォッチドッグ (20 秒無通信) では畳めない —— 攻撃者が
      //     何か送っていれば「活きたリンク」に見えるため。
      //   ★NC の承認待ち中は数えない。そこは人間の操作時間なので、機械の
      //     しびれを切らす時間で測ってはいけない。
      //   ★長さの決め方: 誤爆 (正規の遅いペアリングを切る) の代償は
      //     「strike が溜まって離陸前に 60 秒黙る」で、見逃し (占拠が
      //     45 秒でなく 90 秒続く) の代償はほぼ無い。**非対称なので長めに倒す**。
      if (con_handle != HCI_CON_HANDLE_INVALID && !cmd_authorized &&
          nc_pending_handle == HCI_CON_HANDLE_INVALID &&
          now - conn_started_us > UNAUTH_LINK_TIMEOUT_US) {
        BOARD::diag_printf(
            "[BLE_UART] link unauthorized for %lus — dropping\n",
            (unsigned long)(UNAUTH_LINK_TIMEOUT_US / 1000000ull));
        gap_disconnect(con_handle);
        ++g_pair_strikes;
        conn_started_us = now; // 切断イベントが来るまで連打しない
      }
    }

    // ---- 生存監視 (3 段) ----------------------------------------------
    // 診断 CDC から 'R' で手動起動 (この経路を実機で試すため)。
    if (restart_requested) {
      restart_requested = false;
      bt_full_chip_restart();
      continue;
    }

    // ① 接続ウォッチドッグ: 相手が黙って消えた形を畳む。
    //    ★これをやらないと con_handle が生きたままになり、**再アドバタイズ
    //      しない** = ホストからは永久に見つからない。
    if (con_handle != HCI_CON_HANDLE_INVALID &&
        BOARD::time_us() - last_conn_activity_us > CONN_IDLE_TIMEOUT_US) {
      BOARD::diag_printf("[BLE_UART] link idle %llums — force cleanup\n",
                         (unsigned long long)((BOARD::time_us() -
                                               last_conn_activity_us) / 1000));
      gap_disconnect(con_handle); // 生きていれば正規に切れる。死んでいれば無害
      con_handle = HCI_CON_HANDLE_INVALID;
      tx_notify_enabled = false;
      can_send_requested = false;
      cmd_authorized = false;
      gdb_notify_enabled = false;
      gdb_saw_traffic = false;
      update_gdb_connected();
      ci_nego_stage = 0;
      nc_pending_handle = HCI_CON_HANDLE_INVALID;
      // ★掃除で戻れたかを見張り始める。この gap_disconnect と直後の広告
      //   enable に対して**イベントが 1 つも返らない**なら、掃除では戻らない。
      wd_recovery_pending = true;
      wd_cleanup_us = BOARD::time_us();
      wd_evt_snapshot = hci_event_count;
      last_conn_activity_us = BOARD::time_us();
    }

    // ② エスカレーション: 掃除に応答が無ければコントローラごと無応答。
    if (wd_recovery_pending) {
      if (hci_event_count != wd_evt_snapshot) {
        wd_recovery_pending = false; // 応答あり = 掃除で足りた
      } else if (BOARD::time_us() - wd_cleanup_us > WD_RECOVERY_TIMEOUT_US) {
        wd_recovery_pending = false;
        BOARD::diag_printf("[BLE_UART] controller unresponsive — resetting CYW43\n");
        bt_full_chip_restart();
        continue;
      }
    }

    // ③ 広告の保険: 未接続なのに広告が止まっている事態 (切断イベントの
    //    取りこぼしや enable 失敗) を 1 秒ごとに埋める。enable は冪等。
    //    ★クールダウン中だけは埋めない。ここを無条件にしておくと、上の (d) で
    //      止めた広告を 1 秒後にこの保険が復活させてしまう (対策が自分で
    //      自分を打ち消す形になる)。
    if (con_handle == HCI_CON_HANDLE_INVALID &&
        BOARD::time_us() >= next_adv_ensure_us &&
        advertising_allowed(BOARD::time_us())) {
      gap_advertisements_enable(1);
      next_adv_ensure_us = BOARD::time_us() + 1000000ull;
    }

    // notify 有効でストリームに残りがあるのに要求未発なら補填。
    // ★送り手 (ハブ) はここへは一切触れない — 送信機会の要求を出すのは
    //   この 1ms 補填だけ。旧実装の request_send_if_needed() が
    //   「btstack を突くのは主ループの担当」に倒したのと同じ形。
    // ★★**どちらのチャネルでも**要求を出すこと。ここを NUS の
    //   tx_notify_enabled だけで判定していたため、GDB だけ notify を有効に
    //   した相手 (= まさに GDB クライアント) には CAN_SEND_NOW が一度も
    //   要求されず、stub の返事が出て行かなかった (実機で踏んだ)。
    if (con_handle != HCI_CON_HANDLE_INVALID &&
        (tx_notify_enabled || gdb_notify_enabled) && tx_pending() != 0 &&
        !can_send_requested) {
      can_send_requested = true;
      att_server_request_can_send_now_event(con_handle);
    }

    api(shizuku::object_api::SLEEP_US, 1000);
  }
  return 0;
}

uintptr_t ble_uart_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  g_rx.init();
  g_tx.init();

  uintptr_t failures = api(shizuku::object_api::DECLARE_NAME, (uintptr_t) "ble_uart")
                           .error;
  failures += export_method(method::GET_RX_STREAM, (uintptr_t)&method_get_rx_stream);
  failures += export_method(method::SET_TX_STREAM, (uintptr_t)&method_set_tx_stream);
  failures += export_method(method::SET_GDB_STREAMS,
                            (uintptr_t)&method_set_gdb_streams);
  failures += export_method(method::REQUEST_DISCONNECT,
                            (uintptr_t)&method_request_disconnect);
  failures += export_method(method::GET_OTA_STREAM,
                            (uintptr_t)&method_get_ota_stream);
  failures += export_method(method::GET_CH2_RX_STREAM,
                            (uintptr_t)&method_get_ch2_rx_stream);
  failures += export_method(method::SET_CH2_TX_STREAM,
                            (uintptr_t)&method_set_ch2_tx_stream);
  failures += export_method(method::GET_PAIRING_STATE,
                            (uintptr_t)&method_get_pairing_state);
  failures += export_method(method::PAIRING_ANSWER,
                            (uintptr_t)&method_pairing_answer);
  failures += export_method(method::SET_PAIRING_LOCK,
                            (uintptr_t)&method_set_pairing_lock);
  failures +=
      export_method(method::FORGET_BONDS, (uintptr_t)&method_forget_bonds);
  failures += export_method(method::POLL, (uintptr_t)&poll_loop);

  const auto rx_created =
      api(shizuku::object_api::STREAM_CREATE, (uintptr_t)&g_rx.desc);
  failures += rx_created.error;
  g_rx_stream_id = rx_created.value;

  g_ota_rx.init();
  const auto ota_created =
      api(shizuku::object_api::STREAM_CREATE, (uintptr_t)&g_ota_rx.desc);
  failures += ota_created.error;
  g_ota_stream_id = ota_created.value;

  g_ch2_rx.init();
  const auto ch2_created =
      api(shizuku::object_api::STREAM_CREATE, (uintptr_t)&g_ch2_rx.desc);
  failures += ch2_created.error;
  g_ch2_rx_stream_id = ch2_created.value;

  bt_stack_bringup();
  return failures;
}

} // namespace

uint32_t register_ble_uart(uintptr_t object_id) {
  s_object_id = object_id;
  const uintptr_t OBJECT = object_id;
  // ★OBJECT_ON_CORE(0) は「指定しない」のとは違う — 無指定 (affinity=0) は
  //   「どのコアでもよい」を意味し、SPAWN する poll_loop がスケジューラの都合で
  //   core1 へ乗る余地を残してしまう。LED (peripherals.cpp) が core0 から
  //   cyw43_arch_init() した以上、cyw43 に触れるスレッドは明示的に core0 へ
  //   ピン留めしないと安全ではない。
  const auto created = api(shizuku::object_api::CREATE_OBJECT, OBJECT,
                           (uintptr_t)&ble_uart_main,
                           shizuku::OBJECT_PRIVILEGED | shizuku::OBJECT_ON_CORE(0));
  const auto started = api(shizuku::object_api::CALL_METHOD, OBJECT, 0, 0);
  if (created.error != 0 || started.error != 0 || started.value != 0) {
    BOARD::diag_printf("[BLE_UART] FAILED: create=%lu call=%lu exports_failed=%lu\n",
                       (unsigned long)created.error,
                       (unsigned long)started.error,
                       (unsigned long)started.value);
    return 1;
  }
  BOARD::diag_printf("[BLE_UART] registered (object %lu, rx stream %lu)\n",
                     (unsigned long)OBJECT, (unsigned long)g_rx_stream_id);
  return 0;
}

uint32_t start_ble_uart(uintptr_t object_id) {
  const uintptr_t OBJECT = object_id;
  // ★method 0 (main) を起こしてはいけない — export して戻るだけ。ポーリングは
  //   別スレッドで (apps/thermal.cpp の sampler と同じ形)。
  const auto spawned =
      api(shizuku::object_api::SPAWN, OBJECT, (uintptr_t)method::POLL, 0);
  if (spawned.error != 0) {
    BOARD::diag_printf("[BLE_UART] could not spawn the poll loop (%lu)\n",
                       (unsigned long)spawned.error);
    return 1;
  }
  // cyw43/btstack は preemption-safe ではない (旧実装の budget-0 の理由と同じ) の
  // で、貸し出しは baton (0) にする。
  api(shizuku::object_api::SET_BUDGET, spawned.value, 0);
  BOARD::diag_printf("[BLE_UART] poll thread %lu started\n",
                     (unsigned long)spawned.value);
  return 0;
}

} // namespace ble_uart

} // namespace objects
} // namespace shizuku
