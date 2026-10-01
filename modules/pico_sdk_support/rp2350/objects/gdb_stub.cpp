#include <cstring>
// ===========================================================================
//  GDB stub — プローブ無しで止めて覗く (docs/03_porting_policy.md D40)
// ===========================================================================
//  ★halting debug (SWD) はコアごと止めるが、こちらは DebugMonitor なので
//    **止まるのは対象のスレッドだけ**。他は走り続ける (I-9)。
//
//  ★★入出力は read_byte / write_byte の 2 つに閉じ込めてある。今は USB CDC を
//    直に叩いているが、**本来はストリーム経由にすべき** (2026-08-21 ユーザー指摘:
//    「USB CDC も本当はオブジェクトというかストリーム経由の方が好ましくはある」)。
//    そうなれば診断出力と GDB は 2 本のストリームになり、今の「黙らせる旗」も
//    要らなくなる。差し替えはこの 2 関数だけで済むようにしてある。
//
//  【対応している範囲】GDB が繋がって、止めて、レジスタとメモリを見て、
//    ブレークポイントを置いて、continue / step できるところまで。
//    それ以外の要求には空返事を返す (GDB は「未対応」と解釈する)。
#include "pico/stdlib.h"
#include <cstdio> // snprintf (monitor の返事を組む)
#include "shizuku/objects/usb_cdc.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/gdb_stub.hpp"
#include "shizuku/stream.hpp"

namespace shizuku {
namespace objects {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uint32_t PACKET_MAX = 1024;
constexpr uint32_t REGISTER_COUNT = 17; // r0-r12, sp, lr, pc, xpsr

char g_packet[PACKET_MAX];
char g_reply[PACKET_MAX];
uint32_t g_reply_length = 0;
bool g_attached = false;
// ★stub 自身のスレッド。**絶対に自分を止めない**ための控え。
//   do_continue が「止まったスレッド」へ乗り換える作りなので、これが無いと
//   自分を対象にして自分を止め、GDB チャネルだけ死んで系は生きたまま、という
//   一番分かりにくい壊れ方をする (実際に踏んだ)。
uint32_t g_self_thread = 0xFFFFFFFFu;

uint32_t g_target_thread = 0;
// non-stop モードか (D53)。一覧の出し方と continue の返し方が変わるので、
// 判定より前に要る。
bool g_nonstop = false;
// 誰も繋いでいない間の寝る間隔。★YIELD で回り続けると、デバッグしていない間
//   ずっとスケジューラ 1 周ごとに syscall が挟まる (D48)。
constexpr uint32_t GDB_IDLE_SLEEP_US = 100000; // 100ms
// 繋がれている間、agent が一覧の変化を見に来る間隔。★Z パケットの返事は
//   agent が仕掛け終わるまで待つので、この値がそのまま「置くのにかかる時間」。
constexpr uint32_t AGENT_BUSY_SLEEP_US = 1000; // 1ms
// 置いてあるブレークポイント (FPB の比較器と 1 対 1)。
uintptr_t g_breakpoints[8];
uint32_t g_breakpoint_count = 0;

struct call_result {
  uintptr_t error;
  uintptr_t value;
};
call_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

// ---- 転送 (ここだけがハードウェアに触る) -----------------------------------
// ★★GDB は**自分専用の CDC (channel 1)** を使う。診断 (channel 0) と分けたので、
//   人間向けの文字が握手に混ざりようが無い — D41 の不具合の根がここで消える。
constexpr uint32_t GDB_CHANNEL = 1;

// ★ストリーム転送 (BLE 等)。使わない限り下の 4 関数は今まで通り CDC を叩く
//   — 動いている経路を分岐 1 つ分しか変えない。
constexpr uintptr_t NO_LINK = (uintptr_t)-1;
stream::storage<link_chunk, 8> g_link_in;  // 転送 → stub
stream::storage<link_chunk, 8> g_link_out; // stub → 転送
uintptr_t g_link_in_id = NO_LINK;
uintptr_t g_link_out_id = NO_LINK;
bool g_link_active = false;
volatile bool g_link_connected = false;

enum transport_t {
  TRANSPORT_CDC = 0,
  TRANSPORT_STREAM = 1,
};
transport_t g_current_transport = TRANSPORT_CDC;

// 束を 1 つ剥がして、read_byte がバイトで配るための手持ち。
link_chunk g_in_chunk{};
uint32_t g_in_pos = 0;
link_chunk g_out_chunk{};

int read_byte() {
  // 1) USB CDC ポートを先に確認
  int c = usb_cdc_read(GDB_CHANNEL);
  if (c >= 0) {
    g_current_transport = TRANSPORT_CDC;
    return c;
  }
  // 2) BLE ストリームを確認
  if (g_link_active) {
    if (g_in_pos >= g_in_chunk.len) {
      link_chunk next{};
      if (!g_link_in.hdl().pop(&next))
        return -1;
      g_in_chunk = next;
      g_in_pos = 0;
      if (g_in_chunk.len == 0)
        return -1;
    }
    g_current_transport = TRANSPORT_STREAM;
    return g_in_chunk.data[g_in_pos++];
  }
  return -1;
}

// ★★書いたら**必ず押し出す**。
void flush_out() {
  if (g_current_transport == TRANSPORT_CDC) {
    usb_cdc_flush(GDB_CHANNEL);
    return;
  }
  if (g_out_chunk.len == 0)
    return;
  for (uint32_t spin = 0; spin < 20000; ++spin) {
    if (g_link_out.hdl().available() < g_link_out.desc.capacity)
      break;
    api(object_api::YIELD);
  }
  g_link_out.hdl().push(g_out_chunk);
  g_out_chunk.len = 0;
}

void write_byte(char value) {
  if (g_current_transport == TRANSPORT_CDC) {
    for (uint32_t spin = 0; spin < 20000; ++spin) {
      if (usb_cdc_write_available(GDB_CHANNEL) != 0)
        break;
      usb_cdc_flush(GDB_CHANNEL);
      api(object_api::YIELD);
    }
    usb_cdc_write(GDB_CHANNEL, value);
    return;
  }
  if (g_out_chunk.len >= (uint16_t)sizeof(g_out_chunk.data))
    flush_out();
  g_out_chunk.data[g_out_chunk.len++] = (uint8_t)value;
}

// 「デバッガが繋がっているか」。USB CDC か BLE のどちらかが繋がっていれば true。
bool link_connected() {
  return usb_cdc_connected(GDB_CHANNEL) || usb_cdc_read_available(GDB_CHANNEL) > 0 || (g_link_active && g_link_connected);
}

// ---- 16 進 -----------------------------------------------------------------
char to_hex(uint32_t nibble) {
  return (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}
int from_hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}
void reply_reset() { g_reply_length = 0; }
void reply_char(char value) {
  if (g_reply_length < PACKET_MAX - 1)
    g_reply[g_reply_length++] = value;
}
void reply_text(const char *text) {
  while (*text != '\0')
    reply_char(*text++);
}
void reply_byte_hex(uint8_t value) {
  reply_char(to_hex(value >> 4));
  reply_char(to_hex(value & 0xF));
}
// ★語は**下位バイトから**並べる。GDB の側がそう読むので、ここを間違えると
//   数字が化けたまま「動いているように見える」。
void reply_word_hex(uint32_t value) {
  for (uint32_t index = 0; index < 4; ++index)
    reply_byte_hex((uint8_t)(value >> (8 * index)));
}

// ---- パケット --------------------------------------------------------------
// lead は '$' (普通の返事) か '%' (非同期通知)。★**通知は ack されない** —
//   GDB は '+' を返さず、代わりに vStopped を送ってくる (D53)。
void send_framed(char lead) {
  uint8_t sum = 0;
  write_byte(lead);
  for (uint32_t index = 0; index < g_reply_length; ++index) {
    write_byte(g_reply[index]);
    sum = (uint8_t)(sum + (uint8_t)g_reply[index]);
  }
  write_byte('#');
  write_byte(to_hex(sum >> 4));
  write_byte(to_hex(sum & 0xF));
  flush_out();
}
void send_reply() { send_framed('$'); }

// 1 パケット受け取る。★**どの待ちにも上限を付ける**。上限が無いと、途中で切れた
//   入力ひとつで stub が回り続け、返事もしないまま診断まで道連れに黙る
//   (実際に踏んだ: ログが止まり、GDB にも probe にも何も返らなくなった)。
//   チェックサムが合わなければ '-' を返して再送させる — 化けた要求は実行しない。
// ★上限は**回数ではなく時間**で見る。以前は空振り 30000 回で数えていたが、
//   下で譲るようにした以上、1 周の重さが「素の空回り」から「syscall +
//   スケジューラ 1 周」に変わる — 回数で書くと、実際に待つ時間が桁で動く
//   のに数字は据え置きのまま、という嘘の上限になる。時間で書けば意味が動かない。
//   パケットの途中でこれだけ間が空いたら、それは途切れたということ
//   (CDC 越しのバイトは詰めて来る)。
constexpr uint64_t READ_PATIENCE_US = 50000; // 50ms (CDC)
// ★ストリーム転送 (BLE) はこれでは足りない。CI 15ms の上に notify の
//   スケジュール待ちが乗るので、パケットの途中で 50ms は普通に超える
//   (超えると RSP の 1 パケットが千切れ、GDB からは応答が壊れて見える)。
constexpr uint64_t READ_PATIENCE_LINK_US = 400000; // 400ms

int read_byte_waiting() {
  const uint64_t deadline =
      BOARD::time_us() + (g_link_active ? READ_PATIENCE_LINK_US : READ_PATIENCE_US);
  while (BOARD::time_us() < deadline) {
    const int value = read_byte();
    if (value >= 0)
      return value;
    // ★空振りしている間は**譲る**。ここはパケットの途中 (次の 1 バイトが
    //   来るのを待っている所) なので、他に走れるものがあるなら走らせる。
    //   以前は素の空回りで、待っている間ずっと自分の持ち分を焼いていた。
    api(object_api::YIELD);
  }
  return -1;
}

// first には既に読んだ 1 バイトを渡す。★呼ぶ側が**最初の 1 バイトで黙らせる**
//   ために、そこだけ外に出してある (下の注記を参照)。
bool receive_packet(int first) {
  int value = first;
  while (value != '$') {
    if (value < 0)
      return false; // 何も来ていない (ここだけは即戻る = 譲るため)
    value = read_byte();
  }
  uint32_t length = 0;
  uint8_t sum = 0;
  while (true) {
    value = read_byte_waiting();
    if (value < 0)
      return false; // 途中で切れた。次の '$' から拾い直す
    if (value == '#')
      break;
    if (length >= PACKET_MAX - 1)
      return false;
    g_packet[length++] = (char)value;
    sum = (uint8_t)(sum + (uint8_t)value);
  }
  g_packet[length] = '\0';
  const int high = from_hex((char)read_byte_waiting());
  const int low = from_hex((char)read_byte_waiting());
  if (high < 0 || low < 0)
    return false;
  if (((high << 4) | low) != sum) {
    write_byte('-');
    flush_out();
    return false;
  }
  write_byte('+');
  flush_out();
  return true;
}

// ---- レジスタ --------------------------------------------------------------
// ★GDB の arm m-profile は r0-r12, sp, lr, pc, xpsr の 17 本。並びは決まっている。
uint32_t read_register(KERNEL::CONTEXT *context, uint32_t index) {
  const auto *frame = context->sp;
  switch (index) {
  case 0: return frame->r0;
  case 1: return frame->r1;
  case 2: return frame->r2;
  case 3: return frame->r3;
  case 4: return context->r4;
  case 5: return context->r5;
  case 6: return context->r6;
  case 7: return context->r7;
  case 8: return context->r8;
  case 9: return context->r9;
  case 10: return context->r10;
  case 11: return context->r11;
  case 12: return frame->r12;
  // ★sp は「例外から戻ったあと」の値。退避域の中の値をそのまま出すと、
  //   GDB が見るスタックが 1 フレームぶんずれる。
  case 13: return (uint32_t)ARCH::psp_after_return(*context);
  case 14: return frame->lr;
  case 15: return frame->pc;
  case 16: return frame->xPSR;
  default: return 0;
  }
}


bool write_register(KERNEL::CONTEXT *context, uint32_t index, uint32_t value) {
  if (!context || !context->sp)
    return false;
  auto *frame = context->sp;
  switch (index) {
  case 0: frame->r0 = value; return true;
  case 1: frame->r1 = value; return true;
  case 2: frame->r2 = value; return true;
  case 3: frame->r3 = value; return true;
  case 4: context->r4 = value; return true;
  case 5: context->r5 = value; return true;
  case 6: context->r6 = value; return true;
  case 7: context->r7 = value; return true;
  case 8: context->r8 = value; return true;
  case 9: context->r9 = value; return true;
  case 10: context->r10 = value; return true;
  case 11: context->r11 = value; return true;
  case 12: frame->r12 = value; return true;
  case 14: frame->lr = value; return true;
  case 15: frame->pc = value; return true;
  case 16: frame->xPSR = value; return true;
  default: return false;
  }
}

uint32_t parse_word_hex(const char *&cursor) {
  uint32_t word = 0;
  for (uint32_t index = 0; index < 4; ++index) {
    int high = from_hex(*cursor++);
    int low = from_hex(*cursor++);
    if (high < 0 || low < 0) break;
    word |= (uint32_t)((high << 4) | low) << (8 * index);
  }
  return word;
}

// ---- タイムトラベル (Reverse Debugging / Execution History) ------------------
struct execution_snapshot {
  uint32_t thread_id;
  uint32_t regs[REGISTER_COUNT];
  uint32_t timestamp_us;
};
constexpr uint32_t SNAPSHOT_CAPACITY = 64;
execution_snapshot g_snapshots[SNAPSHOT_CAPACITY];
uint32_t g_snap_head = 0;
uint32_t g_snap_count = 0;
bool g_record_enabled = true;

void record_snapshot(uint32_t thread_id, KERNEL::CONTEXT *context) {
  if (!g_record_enabled || context == nullptr || context->sp == nullptr)
    return;
  auto &snap = g_snapshots[g_snap_head];
  snap.thread_id = thread_id;
  for (uint32_t i = 0; i < REGISTER_COUNT; ++i) {
    snap.regs[i] = read_register(context, i);
  }
  snap.timestamp_us = BOARD::time_us();
  g_snap_head = (g_snap_head + 1) % SNAPSHOT_CAPACITY;
  if (g_snap_count < SNAPSHOT_CAPACITY)
    ++g_snap_count;
}

bool restore_previous_snapshot(uint32_t thread_id, KERNEL::CONTEXT *context) {
  if (g_snap_count == 0 || context == nullptr)
    return false;
  g_snap_head = (g_snap_head + SNAPSHOT_CAPACITY - 1) % SNAPSHOT_CAPACITY;
  --g_snap_count;
  const auto &snap = g_snapshots[g_snap_head];
  for (uint32_t i = 0; i < REGISTER_COUNT; ++i) {
    write_register(context, i, snap.regs[i]);
  }
  return true;
}

// ---- 動的シンボル/変数テーブル ------------------------------------------------
struct dyn_variable {
  char name[24];
  uintptr_t addr;
  uint32_t size;
};
constexpr uint32_t MAX_DYN_VARS = 16;
dyn_variable g_dyn_vars[MAX_DYN_VARS]{};
uint32_t g_dyn_var_count = 0;

// ---- メモリ ----------------------------------------------------------------
// ★どこでも読ませない。範囲外を読むと BusFault になり、**stub 自身が止まる**
//   (そうなると GDB は無応答の相手を待ち続けることになる)。知っている領域だけ許す。
bool address_is_readable(uintptr_t address, uint32_t bytes) {
  const uintptr_t end = address + bytes;
  if (end < address)
    return false;
  const bool in_flash = address >= 0x10000000u && end <= 0x10000000u + 0x400000u;
  const bool in_sram = address >= 0x20000000u && end <= 0x20082000u;
  return in_flash || in_sram;
}

bool address_is_writable(uintptr_t address, uint32_t bytes) {
  const uintptr_t end = address + bytes;
  if (end < address)
    return false;
  const bool in_sram = address >= 0x20000000u && end <= 0x20082000u;
  return in_sram;
}

// ---- 要求ごとの処理 --------------------------------------------------------
// ---- オブジェクト名 (D56) ---------------------------------------------------
// ★「スレッド 7」より「telemetry」の方が分かる。名前は `DECLARE_NAME` で既に
//   カーネルが持っているので、**引くだけ**。
// ★スレッド → オブジェクトは `current_object()` を直に読む (syscall が無い)。
//   呼び出しの途中なら呼ばれている側が返るが、デバッグ表示としてはそれで
//   正しい (今そのスレッドが実行しているのはそちらなので)。
const char *thread_name(uint32_t thread) {
  const uintptr_t object = kernel_object_instance.current_object(thread);
  const auto named = api(object_api::OBJECT_NAME, object);
  if (named.error != 0 || named.value == 0)
    return nullptr;
  return (const char *)named.value;
}

// 文字列を 16 進で積む (qRcmd / qThreadExtraInfo の返事はこの形)。
void reply_hex_text(const char *text) {
  while (text != nullptr && *text != '\0')
    reply_byte_hex((uint8_t)*text++);
}

// ---- 止めてはいけないスレッド (D55 / D56) -----------------------------------
// ★★転送 (BLE / CDC) を回しているスレッドを止めると、**デバッガ自身が
//   喋れなくなる**。止めた本人が復旧できないので、無線だと電源再投入まで
//   戻らない。転送側に申告させて弾く。
// ★★★**stub 自身のスレッド (server 1 + 各コアの agent) もここへ入れる**
//   (D55)。入れないと GDB から `vCont;t:<agent>` でデバッガ自身を止められる:
//     - agent が止まる → 自コアの FPB を誰も面倒見なくなり、以後
//       ブレークポイントの置き直しが効かない。しかも `wait_for_agents()` が
//       毎回 10 万回空回りしてから諦めるので、Z のたびに待たされる
//     - server が止まる → GDB チャネルごと無応答 (系は生きたまま = 一番
//       分かりにくい壊れ方)
//   ★守り方は「止める要求を弾く」ではなく**一覧に出さない**。出さなければ
//     H も vCont も掴めない (どちらも thread_is_listable を通す) ので
//     入口が 1 つで済む。デバッガ自身は**被デバッグ対象ではない**。
//   ★表を 2 つ持たない — 転送スレッドと同じ「止めてはいけない」概念なので
//     同じ表に寄せる (2 つあると必ず片方だけ更新する日が来る)。
// ★★★本来これは **System Object が持つべき制約** (誰が誰を止めてよいか)
//   であって、stub が自前の表で守るのは繋ぎ。資源の階層が入ったら
//   そちらへ移すこと。ここに書いてあるのは「今はまだ無いから」であって、
//   ここが正しい置き場所だからではない。
volatile uint32_t g_protected_threads = 0;

bool thread_is_protected(uint32_t thread) {
  return thread < 32 && (g_protected_threads & (1u << thread)) != 0;
}

uint32_t parse_hex(const char *&cursor) {
  uint32_t value = 0;
  while (from_hex(*cursor) >= 0) {
    value = (value << 4) | (uint32_t)from_hex(*cursor);
    ++cursor;
  }
  return value;
}

// ===========================================================================
//  server ⇄ agent の受け渡し (D49)
// ===========================================================================
//  ★server は**自分では FPB を触らない**。触れるのは対象のコアに居る agent
//    だけなので、「今の一覧を仕掛け直してくれ」と頼む形にする。
//  ★頼み方は世代番号 1 つ。server が上げ、各 agent が自分の適用済み世代を
//    書き戻す。**錠は要らない** — 書く側は server 1 人、各 agent は自分の
//    枠にしか書かないため (席が 1 つずつなのはストリームと同じ考え方)。
//  ★一覧そのもの (g_breakpoints) を書き換えてよいのは server だけ。agent は
//    読むだけ。世代を上げるのは**書き換え終わってから** (release) なので、
//    agent が半端な一覧を読むことはない。
volatile uint32_t g_arm_generation = 0;
volatile uint32_t g_agent_applied[KERNEL::CORE_COUNT] = {};
volatile uint32_t g_agent_alive = 0; // 起きた agent のビット (診断用)

// server 側: 一覧を書き換えたあとに呼ぶ。
void request_rearm() {
  ARCH::store_release32((volatile uint32_t *)&g_arm_generation,
                        g_arm_generation + 1);
}

// server 側: 全 agent が仕掛け終わるまで待つ。★待っている間も譲る。
//   ★**返事をする前に待つ**のが肝。待たずに OK を返すと、GDB は
//     「置けた」と思って continue するのに、実際にはまだ仕掛かっておらず
//     最初の数回を取りこぼす (静かに 1 回目だけ当たらない、という嫌な形)。
void wait_for_agents() {
  const uint32_t want = g_arm_generation;
  for (uint32_t guard = 0; guard < 2000000; ++guard) {
    bool all_done = true;
    for (uint32_t core = 0; core < KERNEL::CORE_COUNT; ++core) {
      // まだ起きていない agent は待たない (待つと永久に返らない)。
      if ((g_agent_alive & (1u << core)) == 0)
        continue;
      if (ARCH::load_acquire32((volatile uint32_t *)&g_agent_applied[core]) !=
          want)
        all_done = false;
    }
    if (all_done)
      return;
    api(object_api::YIELD);
  }
  BOARD::diag_printf("[GDB] warning: an agent did not arm in time\n");
}

// agent 側: 自分のコアの FPB を一覧どおりにする。
// ★ここだけがデバッグハードウェアに触る。**このコアの分しか書けない**ので、
//   同じことを各コアの agent がそれぞれやる。
void __not_in_flash_func(agent_apply_breakpoints)() {
  ARCH::breakpoint_enable(true);
  for (uint32_t index = 0; index < ARCH::breakpoint_count(); ++index)
    ARCH::breakpoint_clear(index);
  for (uint32_t index = 0; index < g_breakpoint_count; ++index)
    ARCH::breakpoint_set(index, g_breakpoints[index]);
}

// ===========================================================================
//  スレッドの見せ方 (D51)
// ===========================================================================
//  ★★GDB のスレッド番号は **1 起点** (0 は「どれでもよい」の意味を持つ) なので、
//    カーネルのスレッド番号 +1 を名乗る。ここでしか変換しない。
uint32_t to_gdb_id(uint32_t thread) { return thread + 1; }
uint32_t from_gdb_id(uint32_t id) { return id - 1; }

// ===========================================================================
//  ★★★一覧に出すのは「**今止まっているスレッド**」だけ (D51)
// ===========================================================================
//  最初は「生きているスレッドを全部出し、走っている相手のレジスタは E01 で
//  断る」形にした。嘘はつかないが、**実 GDB で `info threads` が丸ごと失敗した**
//  (実測): GDB 既定の all-stop は「一覧に居る = 全部止まっている」前提なので、
//  一覧の各スレッドについて無条件にレジスタを読みに行き、1 つでも断られると
//  `Could not read registers; remote failure reply '01'` でコマンドごと落ちる。
//
//  ★このカーネルは「止まるのは対象のスレッドだけ、他は走り続ける」(I-9) ので、
//    そもそも all-stop とは意味論が違う。そこで **GDB へ見せる世界を
//    「止まっているものだけ」に閉じる**: 一覧に出るものは必ず読めるので
//    all-stop の前提と矛盾せず、かつ**走っている相手の古い文脈を
//    「今ここに居る」として見せることも無い**。どちらの嘘もつかない。
//
//  ★見返り: ブレークポイントが複数当たれば、当たった数だけ一覧に増える。
//    「どのスレッドで止まったか」は正しく分かる (以前は thread:1 固定だった)。
//  ★限界 (未実装): **走っているスレッドは見えない**。それを見せるには
//    RSP の non-stop モード (QNonStop + %Stop 通知) が要る — GDB に
//    「一部だけ走っている」と伝える正式な手段はそれだけ。今は未対応。
bool thread_is_stopped(uint32_t thread) {
  return thread < kernel_instance.thread_count() &&
         kernel_instance.thread_state(thread) ==
             KERNEL::THREAD::state_t::SUSPENDED;
}

bool thread_is_alive(uint32_t thread) {
  using state_t = KERNEL::THREAD::state_t;
  if (thread >= kernel_instance.thread_count())
    return false;
  const auto state = kernel_instance.thread_state(thread);
  return state != state_t::UNINITIALIZED && state != state_t::RESERVED &&
         state != state_t::TERMINATED;
}

// ★non-stop なら**走っているものも出す** (GDB が "(running)" と表示し、
//   レジスタを読みに来ない)。all-stop のときだけ「止まっているものだけ」に
//   絞る — あちらは一覧に居る全員のレジスタを無条件に読むため。
bool thread_is_listable(uint32_t thread) {
  // ★デバッガ自身は見せない (D55)。見せると止められる。
  if (thread_is_protected(thread))
    return false;
  return g_nonstop ? thread_is_alive(thread) : thread_is_stopped(thread);
}

// ★★★**止まっているか**。レジスタを見せてよいかの判断はこれだけで決まる。
//   この stub は「止まるのは対象のスレッドだけ、他は走り続ける」(I-9) ので、
//   GDB 既定の all-stop (止まったら全部止まっている) とは意味論が違う。
//   走っているスレッドの文脈は**最後に退避された古い値**でしかないので、
//   それをレジスタとして見せると GDB は古い pc を「今ここに居る」として
//   表示する — **無音の嘘**になる。だから見せずに断る (下の 'g'/'p')。

// ===========================================================================
//  ★★★**デバッガが止めたものだけ**を覚えておく (D52)
// ===========================================================================
//  ブレークポイントが複数当たれば、当たった数だけスレッドが止まる
//  (debug_dispatch は当たった相手を無条件に suspend する)。そうなると
//  **detach で 1 本しか戻さないのは足りない** — 残りは止まったままになり、
//  デバッガを切ったのに系が欠けたまま動き続ける、という分かりにくい壊れ方をする。
//
//  ★「今 SUSPENDED のものを全部戻す」ではいけない。**フォールトで止まった
//    スレッドも SUSPENDED** で (拒否のテストが意図的に 2 本落としている)、
//    それを戻すと落ちた場所へ復帰して延々と落ち続ける。
//    だから**自分が止めたものだけ**を印で持つ。
uint32_t g_debug_stopped = 0; // ビット n = スレッド n を stub が止めた

void mark_stopped(uint32_t thread) {
  if (thread < 32)
    g_debug_stopped |= (1u << thread);
}
void clear_stopped(uint32_t thread) {
  if (thread < 32)
    g_debug_stopped &= ~(1u << thread);
}
// 自分が止めたものを全部戻す (detach / 相手が居なくなった / 切断)。
void resume_all_stopped() {
  for (uint32_t thread = 0; thread < 32; ++thread)
    if (g_debug_stopped & (1u << thread))
      kernel_instance.resume(thread);
  g_debug_stopped = 0;
}

// H で選ばれた相手。0xFFFFFFFF = 指定なし (= 既定の対象を使う)。
constexpr uint32_t NO_SELECTION = 0xFFFFFFFFu;
uint32_t g_selected_query = NO_SELECTION;  // Hg: レジスタ・メモリを見る相手
uint32_t g_selected_resume = NO_SELECTION; // Hc: 走らせる相手

// 実際に見る相手 / 走らせる相手を決める。
uint32_t query_thread() {
  return g_selected_query == NO_SELECTION ? g_target_thread : g_selected_query;
}
uint32_t resume_thread_id() {
  return g_selected_resume == NO_SELECTION ? g_target_thread
                                           : g_selected_resume;
}

// ★★★スレッド ID は **16 進**で書く。RSP の thread-id は一貫して 16 進で、
//   10 進で書くと 1〜9 だけたまたま一致して、**10 本目から静かにずれる**
//   (GDB id 33 は "21" であって "33" ではない)。読む側 (parse_hex) は元から
//   16 進なので、書く側だけ 10 進にすると往復で食い違う。
//   ★詰めない (先頭 0 を付けない) — 桁を固定する規則は無い。
void reply_hex_id(uint32_t value) {
  char digits[9];
  uint32_t length = 0;
  if (value == 0)
    digits[length++] = '0';
  while (value != 0) {
    digits[length++] = to_hex(value & 0xF);
    value >>= 4;
  }
  while (length != 0)
    reply_char(digits[--length]);
}

// 止まったことを知らせる。signal は 5 = SIGTRAP (ブレークポイント / 1 命令実行)、
// 2 = SIGINT (GDB から止めろと言われた = Ctrl-C / VS Code の一時停止)。
// ★どのスレッドで止まったかを**正しく名乗る** (D51)。以前は thread:1 固定で、
//   どれで止まっても GDB には同じ 1 本に見えていた。
constexpr char SIGNAL_TRAP = '5';
constexpr char SIGNAL_INT = '2';

void build_stop_reply(char signal_digit, uint32_t thread) {
  reply_reset();
  reply_text("T0");
  reply_char(signal_digit);
  reply_text("thread:");
  reply_hex_id(to_gdb_id(thread));
  reply_char(';');
}
void stop_reply(char signal_digit, uint32_t thread) {
  build_stop_reply(signal_digit, thread);
  send_reply();
}

// ===========================================================================
//  non-stop モード (D53)
// ===========================================================================
//  ★★★このカーネルは「**止まるのは対象のスレッドだけ、他は走り続ける**」(I-9)。
//    GDB 既定の all-stop は「止まったら全部止まっている」前提なので、意味論が
//    最初から食い違う。all-stop に寄せる (止まったら全部止める) と、この設計の
//    売りをデバッガ接続中だけ捨てることになる — そこで**GDB へ正式に
//    「一部だけ走っている」と伝える**唯一の手段である non-stop を実装した。
//
//  【all-stop との違い】
//    - `vCont;c` は **即 OK を返す** (all-stop では止まるまで無言)
//    - スレッドが止まったら **`%Stop:...` の非同期通知**を送る。通知は '$' では
//      なく '%' で始まり、**GDB は '+' を返さない** — 代わりに `vStopped` で
//      引き取りに来る
//    - 通知は**一度に 1 つだけ**。GDB が `vStopped` で引き取り、こちらが
//      「もう無い」= OK を返すまで、次の通知を送ってはいけない。溜まった分は
//      待ち行列に積んでおく
//    - 一覧には**走っているスレッドも出す** (GDB が "(running)" と表示する)。
//      all-stop のときだけ「止まっているものだけ」に絞る
uint32_t g_pending_mask = 0;      // 通知待ちのスレッド
char g_pending_signal[32] = {};   // その理由
bool g_notify_outstanding = false; // 送った通知をまだ引き取られていない
// ★★★**握手が済むまで通知を送らない** (D53、2026-08-25 実測)。
//   GDB は非同期の準備が整う前に `%Stop` を受け取ると
//     remote.c: internal-error: mark_async_event_handler:
//     Assertion `this->is_async_p ()' failed
//   で**落ちる**。実際、`QNonStop:1` の返事に通知がくっついて出ていた
//   (`+$OK#9a%Stop:T05thread:1;#b7`)。GDB が最初の `?` を聞いて
//   「今どうなっているか」を受け取るまでは、通知ではなくその返事で伝える。
bool g_notify_allowed = false;

void queue_stop(uint32_t thread, char signal_digit) {
  if (thread >= 32)
    return;
  g_pending_mask |= (1u << thread);
  g_pending_signal[thread] = signal_digit;
}

// 待ち行列の先頭を取り出して reply へ組む。無ければ false。
bool take_pending(bool consume) {
  for (uint32_t thread = 0; thread < 32; ++thread) {
    if ((g_pending_mask & (1u << thread)) == 0)
      continue;
    build_stop_reply(g_pending_signal[thread], thread);
    if (consume)
      g_pending_mask &= ~(1u << thread);
    return true;
  }
  return false;
}

// 溜まっていて、まだ通知を出していないなら 1 つ出す。
void notify_if_needed() {
  if (!g_nonstop || !g_notify_allowed || g_notify_outstanding ||
      g_pending_mask == 0)
    return;
  reply_reset();
  reply_text("Stop:");
  // ★通知の中身は停止応答そのもの。**待ち行列からは消さない** — 消すのは
  //   GDB が vStopped で引き取ったとき (取りこぼすとそのスレッドは
  //   「止まったのに GDB が知らない」まま残る)。
  for (uint32_t thread = 0; thread < 32; ++thread) {
    if ((g_pending_mask & (1u << thread)) == 0)
      continue;
    reply_text("T0");
    reply_char(g_pending_signal[thread]);
    reply_text("thread:");
    reply_hex_id(to_gdb_id(thread));
    reply_char(';');
    break;
  }
  send_framed('%');
  g_notify_outstanding = true;
}

// ===========================================================================
//  走らせている間も stub は主ループへ戻る (D48)
// ===========================================================================
//  ★★★以前は continue / step のあと **止まるまでこの中で待って**いた。それが
//    2 つの実害を生んでいた:
//    (a) **GDB の「止めろ」が効かない**。RSP では走行中の相手を止めるとき、
//        GDB は裸の 0x03 を 1 バイト送る (Ctrl-C / VS Code の一時停止ボタン)。
//        待ちの中に居ると CDC を一切読まないので、それが無視される
//    (b) **待ちきれないと嘘をついていた**。上限 (10 万回) を超えたら「止まった」
//        ことにして T05 を返していたので、実際は走り続けている相手について
//        GDB が古い pc で「停止しました」と表示する。**無音の嘘**そのもので、
//        この repo が一番やってはいけないと決めていること
//  → 走らせたら**返事をせずに主ループへ戻る**。停止応答を送るのは、
//    (1) 本当に止まった (debug_event_count が動いた) か、
//    (2) GDB から 0x03 が来た、のどちらかが起きたとき**だけ**。
//    上限は要らない — 待たないので、止まらない相手が居ても stub は回り続ける。
bool g_running = false;         // 走らせていて、GDB へ停止応答を借りている
uint32_t g_resume_count = 0;    // 走らせた時点の debug_event_count

// 走らせる。★返事はしない (主ループが、止まったときに送る)。
// ★H / vCont で相手が指定されていればそれを走らせる (D51)。
void resume_target() {
  g_resume_count = kernel_instance.debug_event_count();
  g_running = true;
  const uint32_t target = resume_thread_id();
  // ★自分は走らせない (自分は既に走っている)。指定が自分なら既定へ戻す。
  if (!thread_is_protected(target))
    g_target_thread = target;
  clear_stopped(g_target_thread); // 走らせたので、もう自分が止めてはいない
  kernel_instance.resume(g_target_thread);
}

void do_continue() { resume_target(); }

// ★★以前はここで直接 ARCH::debug_step(true) を立てていたが、それだと
//   「次の 1 命令」は**この stub 自身の次の命令**になってしまい、対象では
//   なく stub 自身が single-step で止まっていた (自己停止のガードは
//   do_continue にしか無いので、そのまま GDB チャネルごと死んでいた —
//   これが「2 回目以降の continue/stepi が固まる」の実体だった。
//   docs/05_handoff.md の指示通り、生ログと debug_dispatch の実測で特定した)。
//   MON_STEP は kernel_instance.arm_step で**予約**するだけにし、実際に
//   立てるのは対象の文脈へ復帰する直前 (CTX_RESTORE) — カーネル側に移した。
void do_step() {
  const uint32_t target = resume_thread_id();
  if (!thread_is_protected(target))
    g_target_thread = target;
  KERNEL::CONTEXT *ctx = kernel_instance.thread_context(g_target_thread);
  record_snapshot(g_target_thread, ctx);
  kernel_instance.arm_step(g_target_thread);
  resume_target();
}

// 走らせている間に、止まったかどうかを見る。止まっていれば停止応答を送る。
// ★主ループから毎周見る。**ここでは待たない**。
// ★★★non-stop では **g_running でなくても**見る (D53)。誰かが勝手に
//   ブレークポイントへ当たることが普通にあり (走らせた覚えが無い相手でも、
//   仕掛けてあれば当たる)、それを取りこぼすと「止まっているのに GDB は
//   走っていると思っている」ズレが残る。
//   ★取りこぼしの限界: カーネルが持つ記録は**直近 1 件**なので、2 本が
//     ほぼ同時に止まると古い方を見落とす。見る間隔は短い (譲るたび) ので
//     実用上はまず起きないが、機構としては 1 件ぶんの窓がある。
bool poll_for_stop() {
  if (!g_running && !g_nonstop)
    return false;
  if (kernel_instance.debug_event_count() == g_resume_count)
    return false; // 新しい停止は起きていない
  g_resume_count = kernel_instance.debug_event_count(); // 見た印を進める
  const uint32_t stopped = kernel_instance.debug_event().thread;
  // ★自分が止まったのなら、乗り換えない (乗り換えると次に自分を止めて黙る)。
  if (!thread_is_protected(stopped)) {
    g_target_thread = stopped;
    mark_stopped(stopped); // detach で戻す相手として覚えておく
  } else {
    kernel_instance.resume(stopped);
  }
  g_running = false;
  // ★止まった相手を**そのまま**名乗る (D51)。GDB の call stack はこれで
  //   正しいスレッドを指す。
  if (g_nonstop) {
    // ★non-stop では**返事ではなく通知**で知らせる (D53)。continue には
    //   既に OK を返してある。
    queue_stop(g_target_thread, SIGNAL_TRAP);
    notify_if_needed();
  } else {
    stop_reply(SIGNAL_TRAP, g_target_thread);
  }
  return true;
}

// GDB から「止めろ」(0x03) が来たとき。★止めてから応答する — 順序を逆にすると
//   GDB は止まったと思って続きを送るのに、相手はまだ走っている。
void interrupt_target() {
  if (!thread_is_protected(g_target_thread)) {
    kernel_instance.suspend(g_target_thread);
    mark_stopped(g_target_thread);
  }
  g_running = false;
  if (g_nonstop) {
    queue_stop(g_target_thread, SIGNAL_INT);
    notify_if_needed();
  } else {
    stop_reply(SIGNAL_INT, g_target_thread);
  }
}


// ===========================================================================
//  ターゲット記述 (D53)
// ===========================================================================
//  ★★★これを配らないと、GDB は**レジスタの並びを自分で推測する**。推測が
//    外れると `Truncated register 16 in remote 'g' packet` になる — こちらは
//    17 本 (r0-r12, sp, lr, pc, xpsr) を 136 文字ちょうどで返しているのに、
//    GDB 側は旧来の ARM 配置 (FPA レジスタ入り) を期待して足りないと言う。
//  ★★実測で厄介だったのは、**all-stop では通り non-stop でだけ落ちた**こと
//    (同じ 136 文字を、片方は受け取り片方は拒む)。GDB がアーキテクチャを
//    確定する順序がモードで違うためで、`set architecture armv8-m.main` を
//    明示しても直らなかった。**推測させないのが唯一の直し方**。
//  ★中身に RSP のエスケープが要る文字 ($ # } *) は入れないこと。
constexpr char TARGET_XML[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/>"
    "<reg name=\"r1\" bitsize=\"32\"/>"
    "<reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/>"
    "<reg name=\"r4\" bitsize=\"32\"/>"
    "<reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/>"
    "<reg name=\"r7\" bitsize=\"32\"/>"
    "<reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/>"
    "<reg name=\"r10\" bitsize=\"32\"/>"
    "<reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\" regnum=\"16\"/>"
    "</feature></target>";

// qXfer:features:read:target.xml:OFFSET,LENGTH に答える。
// 先頭が 'm' = まだ続く / 'l' = これで終わり。
void reply_target_xml(const char *cursor) {
  // annex (target.xml) を読み飛ばして OFFSET,LENGTH を取る。
  while (*cursor != ':' && *cursor != '\0')
    ++cursor;
  if (*cursor == ':')
    ++cursor;
  const uint32_t offset = parse_hex(cursor);
  if (*cursor == ',')
    ++cursor;
  uint32_t length = parse_hex(cursor);

  const uint32_t total = (uint32_t)(sizeof(TARGET_XML) - 1);
  if (offset >= total) {
    reply_text("l");
    return;
  }
  uint32_t remain = total - offset;
  // ★返事そのものにも上限がある。PacketSize より小さく抑える。
  if (length > PACKET_MAX - 64)
    length = PACKET_MAX - 64;
  const bool last = remain <= length;
  if (!last)
    remain = length;
  reply_text(last ? "l" : "m");
  for (uint32_t index = 0; index < remain; ++index)
    reply_char(TARGET_XML[offset + index]);
}

// ---- monitor (qRcmd) — 覗く相手を実行時に選ぶ (D56) -------------------------
//  `monitor list`        一覧 (番号・名前・状態)
//  `monitor target <n>`  その番号を対象にする (止めて、以後の既定にする)
//  `monitor help`
// ★返事は 16 進のテキスト。GDB がそれを人間向けに出す。
// ★★対象を変えたあと GDB の表示が追いつくのは**次に止まったとき**。
//   VS Code なら `monitor target 7` のあと一時停止ボタンを押せば、7 番が
//   call stack に出る (押した時点の対象が 7 になっているため)。
void handle_monitor(const char *hex) {
  char command[128];
  uint32_t length = 0;
  while (length + 1 < sizeof(command) && from_hex(hex[0]) >= 0 &&
         from_hex(hex[1]) >= 0) {
    command[length++] =
        (char)((from_hex(hex[0]) << 4) | (uint32_t)from_hex(hex[1]));
    hex += 2;
  }
  command[length] = '\0';
  // 前後の空白を落とす
  const char *at = command;
  while (*at == ' ')
    ++at;

  // ---- 1) メモリ確保 (alloc <size>) ----
  if (__builtin_strncmp(at, "alloc", 5) == 0) {
    const char *arg = at + 5;
    while (*arg == ' ') ++arg;
    uint32_t size = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
      arg += 2;
      while (from_hex(*arg) >= 0) size = (size << 4) | (uint32_t)from_hex(*arg++);
    } else {
      while (*arg >= '0' && *arg <= '9') size = size * 10 + (uint32_t)(*arg++ - '0');
    }
    if (size == 0) size = 4;
    const auto res = api(object_api::MEMORY_ALLOCATE, size);
    char line[64];
    if (res.error != 0 || res.value == 0) {
      snprintf(line, sizeof(line), "alloc failed (error %lu)\n", (unsigned long)res.error);
    } else {
      snprintf(line, sizeof(line), "0x%08lx\n", (unsigned long)res.value);
    }
    reply_hex_text(line);
    return;
  }

  // ---- 2) メモリ解放 (free <addr>) ----
  if (__builtin_strncmp(at, "free", 4) == 0) {
    const char *arg = at + 4;
    while (*arg == ' ') ++arg;
    uintptr_t addr = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) arg += 2;
    while (from_hex(*arg) >= 0) addr = (addr << 4) | (uintptr_t)from_hex(*arg++);
    const auto res = api(object_api::MEMORY_RELEASE, addr);
    char line[64];
    if (res.error != 0) {
      snprintf(line, sizeof(line), "free failed (error %lu)\n", (unsigned long)res.error);
    } else {
      snprintf(line, sizeof(line), "freed 0x%08lx\n", (unsigned long)addr);
    }
    reply_hex_text(line);
    return;
  }

  // ---- 3) 動的スレッド/オブジェクト生成 (spawn <entry_hex|obj_id> [arg1]) ----
  if (__builtin_strncmp(at, "spawn", 5) == 0) {
    const char *arg = at + 5;
    while (*arg == ' ') ++arg;
    uintptr_t entry_or_id = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
      arg += 2;
      while (from_hex(*arg) >= 0) entry_or_id = (entry_or_id << 4) | (uintptr_t)from_hex(*arg++);
    } else {
      while (*arg >= '0' && *arg <= '9') entry_or_id = entry_or_id * 10 + (uintptr_t)(*arg++ - '0');
    }
    while (*arg == ' ') ++arg;
    uintptr_t a1 = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
      arg += 2;
      while (from_hex(*arg) >= 0) a1 = (a1 << 4) | (uintptr_t)from_hex(*arg++);
    } else {
      while (*arg >= '0' && *arg <= '9') a1 = a1 * 10 + (uintptr_t)(*arg++ - '0');
    }
    char line[96];
    uintptr_t obj_id = entry_or_id;
    if (entry_or_id >= 0x10000000) {
      static uint32_t s_dyn_obj = 24;
      obj_id = s_dyn_obj++;
      if (s_dyn_obj >= 32) s_dyn_obj = 24;
      // ★OBJECT_REPLACE で、再利用時も methods[0] を新しい entry へ差し替える
      //   (特権オブジェクトだけが立てられる。gdb_stub は特権)。
      const auto c_res = api(object_api::CREATE_OBJECT, obj_id, entry_or_id,
                             OBJECT_REPLACE | 0x100u);
      if (c_res.error != 0) {
        snprintf(line, sizeof(line), "create_object failed (%lu)\n", (unsigned long)c_res.error);
        reply_hex_text(line);
        return;
      }
      asm volatile("dsb\nisb" ::: "memory");
    }
    const auto s_res = api(object_api::SPAWN, obj_id, a1, 0);
    if (s_res.error != 0) {
      snprintf(line, sizeof(line), "spawn failed (error %lu)\n", (unsigned long)s_res.error);
    } else {
      g_target_thread = (uint32_t)s_res.value;
      snprintf(line, sizeof(line), "spawned thread %lu (object %lu)\n", (unsigned long)s_res.value, (unsigned long)obj_id);
    }
    reply_hex_text(line);
    return;
  }

  // ---- 4) メソッド呼び出し (call <obj_id> <method_id> [arg]) ----
  if (__builtin_strncmp(at, "call", 4) == 0) {
    const char *arg = at + 4;
    while (*arg == ' ') ++arg;
    uintptr_t obj_id = 0, method = 0, argument = 0;
    while (*arg >= '0' && *arg <= '9') obj_id = obj_id * 10 + (uintptr_t)(*arg++ - '0');
    while (*arg == ' ') ++arg;
    while (*arg >= '0' && *arg <= '9') method = method * 10 + (uintptr_t)(*arg++ - '0');
    while (*arg == ' ') ++arg;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
      arg += 2;
      while (from_hex(*arg) >= 0) argument = (argument << 4) | (uintptr_t)from_hex(*arg++);
    } else {
      while (*arg >= '0' && *arg <= '9') argument = argument * 10 + (uintptr_t)(*arg++ - '0');
    }
    const auto res = api(object_api::CALL_METHOD, obj_id, method, argument);
    char line[96];
    snprintf(line, sizeof(line), "call result: %lu (error: %lu)\n", (unsigned long)res.value, (unsigned long)res.error);
    reply_hex_text(line);
    return;
  }
  // ---- 5) 動的変数登録 (var <name>=<addr> [size]) ----
  if (__builtin_strncmp(at, "var ", 4) == 0) {
    const char *arg = at + 4;
    while (*arg == ' ') ++arg;
    char name[24]{};
    uint32_t n_len = 0;
    while (*arg != '=' && *arg != ' ' && *arg != ' ' && n_len + 1 < sizeof(name)) {
      name[n_len++] = *arg++;
    }
    while (*arg == ' ' || *arg == '=') ++arg;
    uintptr_t addr = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) arg += 2;
    while (from_hex(*arg) >= 0) addr = (addr << 4) | (uintptr_t)from_hex(*arg++);
    while (*arg == ' ') ++arg;
    uint32_t size = 4;
    if (*arg != ' ') {
      size = 0;
      while (*arg >= '0' && *arg <= '9') size = size * 10 + (uint32_t)(*arg++ - '0');
    }
    // Update or insert
    int slot = -1;
    for (uint32_t i = 0; i < g_dyn_var_count; ++i) {
      if (__builtin_strcmp(g_dyn_vars[i].name, name) == 0) { slot = i; break; }
    }
    if (slot < 0 && g_dyn_var_count < MAX_DYN_VARS) slot = g_dyn_var_count++;
    char line[96];
    if (slot >= 0) {
      __builtin_strncpy(g_dyn_vars[slot].name, name, sizeof(g_dyn_vars[slot].name) - 1);
      g_dyn_vars[slot].addr = addr;
      g_dyn_vars[slot].size = size;
      snprintf(line, sizeof(line), "registered $%s = 0x%08lx (%lu bytes)\n", name, (unsigned long)addr, (unsigned long)size);
    } else {
      snprintf(line, sizeof(line), "variable table full\n");
    }
    reply_hex_text(line);
    return;
  }

  // ---- 6) 動的変数一覧 (vars) ----
  if (__builtin_strncmp(at, "vars", 4) == 0) {
    if (g_dyn_var_count == 0) {
      reply_hex_text("No dynamic variables registered.\n");
      return;
    }
    char line[128];
    for (uint32_t i = 0; i < g_dyn_var_count; ++i) {
      snprintf(line, sizeof(line), "[%lu] $%s @ 0x%08lx (size: %luB)\n",
               (unsigned long)i, g_dyn_vars[i].name,
               (unsigned long)g_dyn_vars[i].addr,
               (unsigned long)g_dyn_vars[i].size);
      reply_hex_text(line);
    }
    return;
  }

  // ---- 7) 巻き戻し (rewind [N]) ----
  if (__builtin_strncmp(at, "rewind", 6) == 0) {
    const char *arg = at + 6;
    while (*arg == ' ') ++arg;
    uint32_t steps = 1;
    if (*arg >= '0' && *arg <= '9') {
      steps = 0;
      while (*arg >= '0' && *arg <= '9') steps = steps * 10 + (uint32_t)(*arg++ - '0');
    }
    uint32_t done = 0;
    KERNEL::CONTEXT *ctx = kernel_instance.thread_context(g_target_thread);
    for (uint32_t s = 0; s < steps && g_snap_count > 0; ++s) {
      if (restore_previous_snapshot(g_target_thread, ctx))
        ++done;
    }
    char line[96];
    if (done > 0 && ctx != nullptr) {
      snprintf(line, sizeof(line), "rewound %lu steps -> PC: 0x%08lx (Thread %lu)\n",
               (unsigned long)done, (unsigned long)read_register(ctx, 15), (unsigned long)g_target_thread);
    } else {
      snprintf(line, sizeof(line), "no recorded history to rewind\n");
    }
    reply_hex_text(line);
    return;
  }

  // ---- 8) 実行履歴 (history) ----
  if (__builtin_strncmp(at, "history", 7) == 0) {
    char line[96];
    snprintf(line, sizeof(line), "Recorded snapshots: %lu / %lu\n",
             (unsigned long)g_snap_count, (unsigned long)SNAPSHOT_CAPACITY);
    reply_hex_text(line);
    return;
  }

  // ---- 9) 記録モード設定 (record on|off|clear) ----
  if (__builtin_strncmp(at, "record", 6) == 0) {
    const char *arg = at + 6;
    while (*arg == ' ') ++arg;
    if (__builtin_strncmp(arg, "off", 3) == 0) {
      g_record_enabled = false;
      reply_hex_text("record disabled\n");
    } else if (__builtin_strncmp(arg, "clear", 5) == 0) {
      g_snap_count = 0;
      g_snap_head = 0;
      reply_hex_text("history cleared\n");
    } else {
      g_record_enabled = true;
      reply_hex_text("record enabled\n");
    }
    return;
  }


  if (__builtin_strncmp(at, "list", 4) == 0) {
    char line[96];
    const uint32_t count = kernel_instance.thread_count();
    for (uint32_t thread = 0; thread < count && thread < 32; ++thread) {
      if (!thread_is_alive(thread))
        continue;
      const char *name = thread_name(thread);
      const int written = snprintf(
          line, sizeof(line), "%3lu  %-14s %-8s%s%s\n",
          (unsigned long)to_gdb_id(thread), name != nullptr ? name : "(no name)",
          thread_is_stopped(thread) ? "stopped" : "running",
          thread == g_target_thread ? "  <- target" : "",
          thread_is_protected(thread) ? "  [transport: 止められない]" : "");
      if (written > 0)
        reply_hex_text(line);
    }
    return;
  }

  if (__builtin_strncmp(at, "target", 6) == 0) {
    const char *argument = at + 6;
    while (*argument == ' ')
      ++argument;
    if (*argument == '\0') {
      reply_hex_text("usage: monitor target <thread|name>\n");
      return;
    }
    char line[96];
    uint32_t id = 0;
    if (*argument >= '0' && *argument <= '9') {
      // ★10 進で受ける (一覧に出している番号と同じ形で打てるように)。
      while (*argument >= '0' && *argument <= '9')
        id = id * 10 + (uint32_t)(*argument++ - '0');
    } else {
      // ★★**名前でも受ける** (D56)。スレッド番号は起こす順で決まるので
      //   ビルドが変われば動く。`monitor target telemetry` と書けた方が、
      //   launch.json に固定で書いても腐らない。
      // ★★名前は**スレッドを一意に指さない**。`thread_name()` が返すのは
      //   `current_object()` = 影スタックの先端で、「今どのオブジェクトとして
      //   走っているか」でしかない。同じオブジェクトのスレッドが 2 本あれば
      //   2 本とも同じ名前で並ぶし、呼び出しの中に居れば呼ばれている側の
      //   名前になる。**黙って先頭を取ると、頼まれたのと違う相手を止める**
      //   ので、曖昧なら止めずに番号を見せて選び直させる。
      const uint32_t count = kernel_instance.thread_count();
      uint32_t matches = 0;
      for (uint32_t thread = 0; thread < count && thread < 32; ++thread) {
        if (!thread_is_alive(thread) || thread_is_protected(thread))
          continue;
        const char *name = thread_name(thread);
        if (name != nullptr && __builtin_strcmp(name, argument) == 0) {
          if (matches == 0)
            id = to_gdb_id(thread);
          ++matches;
        }
      }
      if (matches == 0) {
        if (snprintf(line, sizeof(line), "no object named '%s'\n", argument) > 0)
          reply_hex_text(line);
        return;
      }
      if (matches > 1) {
        if (snprintf(line, sizeof(line),
                     "'%s' matches %lu threads — 番号で指定してください "
                     "(monitor list)\n",
                     argument, (unsigned long)matches) > 0)
          reply_hex_text(line);
        return;
      }
    }
    const uint32_t thread = from_gdb_id(id);

    if (id == 0 || thread >= kernel_instance.thread_count() ||
        !thread_is_alive(thread)) {
      if (snprintf(line, sizeof(line), "no such thread: %lu\n",
                   (unsigned long)id) > 0)
        reply_hex_text(line);
      return;
    }
    // ★★止めてはいけない相手は弾く。踏むとデバッガ自身が黙るので、
    //   「やってみて壊れる」に任せてはいけない種類の操作。
    if (thread_is_protected(thread)) {
      reply_hex_text("that is the stub itself\n");
      return;
    }
    if (thread_is_protected(thread)) {
      if (snprintf(line, sizeof(line),
                   "%lu is the debug transport — 止めると自分が喋れなくなる\n",
                   (unsigned long)id) > 0)
        reply_hex_text(line);
      return;
    }

    g_target_thread = thread;
    // 選び直したので、H の選択は白紙に戻す (古い相手を見続けないため)。
    g_selected_query = NO_SELECTION;
    g_selected_resume = NO_SELECTION;
    if (!thread_is_stopped(thread)) {
      kernel_instance.suspend(thread);
      mark_stopped(thread);
      queue_stop(thread, SIGNAL_TRAP);
      notify_if_needed();
    }
    const char *name = thread_name(thread);
    if (snprintf(line, sizeof(line), "target is now %lu (%s)\n",
                 (unsigned long)id, name != nullptr ? name : "(no name)") > 0)
      reply_hex_text(line);
    return;
  }

  reply_hex_text("monitor list                  一覧 (番号・名前・状態)\n"
                 "monitor target <n|name>       覗く相手にする\n"
                 "monitor alloc <bytes>         実機RAMに動的メモリ確保\n"
                 "monitor free <addr>           実機メモリ解放\n"
                 "monitor spawn <entry|id> [arg]動的スレッド生成\n"
                 "monitor call <id> <m> [arg]   オブジェクトメソッド実行\n");
}

void handle_packet() {
  const char *cursor = g_packet;
  // ★見る相手は H で選ばれたもの (D51)。走らせる相手 (g_target_thread) とは
  //   別に持つ — GDB は「止まっている A を見ながら B を走らせる」を普通にやる。
  const uint32_t inspected = query_thread();
  KERNEL::CONTEXT *context = kernel_instance.thread_context(inspected);
  reply_reset();
  switch (*cursor++) {
  case 'q':
    if (__builtin_strncmp(cursor, "Xfer:features:read:", 19) == 0)
      reply_target_xml(cursor + 19);
    else if (__builtin_strncmp(cursor, "Supported", 9) == 0)
      reply_text("PacketSize=3ff;QNonStop+;qXfer:features:read+;ReverseStep+;ReverseContinue+;");
    else if (__builtin_strncmp(cursor, "Attached", 8) == 0)
      reply_text("1");
    // ★スレッド一覧に添える説明。GDB は繋いだ直後にこれを聞いてくるので、
    //   返してやれば `info threads` にも VS Code の CALL STACK にも
    //   **オブジェクト名**が出る (以前は空を返していたので番号だけだった)。
    else if (__builtin_strncmp(cursor, "ThreadExtraInfo,", 16) == 0) {
      const char *at = cursor + 16;
      const uint32_t thread = from_gdb_id(parse_hex(at));
      const char *name = thread_name(thread);
      reply_hex_text(name != nullptr ? name : "(no name)");
      reply_hex_text(thread_is_stopped(thread) ? " [stopped]" : " [running]");
      if (thread == g_target_thread)
        reply_hex_text(" *target*");
      if (thread_is_protected(thread))
        reply_hex_text(" [transport]");
    }
    // ★`monitor ...` (D56)。**どのオブジェクトを覗くかを実行時に選ぶ**ための口。
    //   all-stop では「止まっているものだけ」が GDB に見えるので、対象を
    //   選べないと合成時に決めた 1 本しか触れない。VS Code からも
    //   `-exec monitor target 7` で撃てる。
    else if (__builtin_strncmp(cursor, "Rcmd,", 5) == 0)
      handle_monitor(cursor + 5);
    else if (*cursor == 'C') {
      // 今どれを見ているか。
      reply_text("QC");
      reply_hex_id(to_gdb_id(query_thread()));
    }
    // ★スレッドの一覧を返さないと、GDB は「走っているスレッドが無い」と判断して
    //   [Thread 1 exited] と言って何もできなくなる (実際にそうなった)。
    //   f = 最初の一括、s = 続き (l = これで終わり)。
    // ★★**生きているものを全部**返す (D51)。以前は "m1" 固定で、カーネルが
    //   何本走らせていても GDB には 1 本しか見えていなかった。スレッドを
    //   持つのがこのカーネルの主題なので、そこを隠したままでは
    //   デバッガとして用を成さない。PacketSize=3ff (1023B) に対して
    //   32 本 × 数文字なので一度に収まる。
    else if (__builtin_strncmp(cursor, "fThreadInfo", 11) == 0) {
      bool first = true;
      for (uint32_t thread = 0; thread < kernel_instance.thread_count();
           ++thread) {
        if (!thread_is_listable(thread))
          continue;
        reply_char(first ? 'm' : ',');
        reply_hex_id(to_gdb_id(thread));
        first = false;
      }
      if (first)
        reply_text("l"); // 1 本も居ない (起動の最初期)
    } else if (__builtin_strncmp(cursor, "sThreadInfo", 11) == 0)
      reply_text("l");
    break;
  case 'T': { // そのスレッドは生きているか
    const uint32_t id = parse_hex(cursor);
    // ★生きていないなら**そう答える**。OK を返し続けると、GDB は終わった
    //   スレッドを掴んだまま離さない。
    reply_text(thread_is_listable(from_gdb_id(id)) ? "OK" : "E01");
    break;
  }
  case '?':
    // ★どのスレッドで止まっているかまで言う。S05 だけだと GDB がスレッドを
    //   結び付けられない。
    // ★★non-stop では「止まっている分を 1 つ返し、続きは vStopped で」
    //   という約束 (D53)。止まっているものが無ければ OK。
    if (g_nonstop) {
      // 今止まっているものを全部待ち行列へ入れ直してから先頭を返す。
      // ★★★**印ではなく実際の状態から答える** (D53)。以前は
      //   「自分が止めたつもり」の印 (g_debug_stopped) を見ていたが、それは
      //   実状態とズレる (印を付けた後に走り出す経路がある)。ズレると
      //   `?` で「止まっている」と名乗った相手について `g` が E01 を返し、
      //   GDB は接続直後に `Could not read registers` で落ちる
      //   (実測で踏んだ: `? -> T05thread:1;` の直後に `g -> E01`)。
      //   **名乗る条件と見せられる条件を同じものにする**。
      for (uint32_t thread = 0; thread < 32; ++thread)
        if (thread_is_stopped(thread) && !thread_is_protected(thread))
          queue_stop(thread, SIGNAL_TRAP); // D55: デバッガ自身は報告しない
      if (take_pending(false)) {
        g_notify_outstanding = true; // 続きは vStopped で引き取らせる
        g_notify_allowed = true;     // ここから先は通知でよい
        send_reply();
        return;
      }
      g_notify_allowed = true;
      reply_reset();
      reply_text("OK");
      break;
    }
    reply_reset();
    reply_text("T05thread:");
    reply_hex_id(to_gdb_id(g_target_thread));
    reply_char(';');
    break;
  case 'H': {
    // ★スレッドの選択 (D51)。以前は受けて**捨てて**いたので、GDB が
    //   「この相手を見る/走らせる」と言っても常に既定の 1 本を見ていた。
    //   Hg… = レジスタ・メモリを見る相手 / Hc… = 走らせる相手。
    const char which = *cursor++;
    uint32_t *slot = (which == 'c') ? &g_selected_resume : &g_selected_query;
    if (*cursor == '-') {
      // -1 = 「全部」。この stub は 1 本ずつしか止められないので、
      // 既定の対象に戻す (嘘をつくより既定へ倒す)。
      *slot = NO_SELECTION;
    } else {
      const uint32_t id = parse_hex(cursor);
      // 0 = 「どれでもよい」。指定が生きていなければ既定へ倒す。
      *slot = (id == 0 || !thread_is_listable(from_gdb_id(id)))
                  ? NO_SELECTION
                  : from_gdb_id(id);
    }
    reply_text("OK");
    break;
  }
  case 'Q':
    // ★QNonStop:1 = non-stop へ入る (D53)。GDB は qSupported で
    //   QNonStop+ を見てから聞いてくる。
    if (__builtin_strncmp(cursor, "NonStop:", 8) == 0) {
      g_nonstop = (cursor[8] == '1');
      g_pending_mask = 0;
      g_notify_outstanding = false;
      g_notify_allowed = false; // 最初の '?' を聞かれるまで通知しない
      // ★古い停止事象を持ち込まない。ここを揃えないと、モードを切り替えた
      //   瞬間に**過去の停止**が通知として飛び出す (それが GDB を殺していた)。
      g_resume_count = kernel_instance.debug_event_count();
      BOARD::diag_printf("[GDB] %s mode\n", g_nonstop ? "non-stop" : "all-stop");
    }
    reply_text("OK");
    break;
  // ★★★レジスタは**止まっている相手のぶんしか見せない** (D51)。
  //   この stub は「止まるのは対象のスレッドだけ」なので、走っている相手の
  //   文脈は最後に退避された古い値でしかない。それを返すと GDB は古い pc を
  //   「今ここに居る」として表示する — **無音の嘘**になる。断る方を選ぶ。
  //   (メモリ 'm' は共有なのでスレッドに依らず読める。下はそのまま。)
  case 'g':
    if (context == nullptr || !thread_is_stopped(inspected))
      reply_text("E01");
    else
      for (uint32_t index = 0; index < REGISTER_COUNT; ++index)
        reply_word_hex(read_register(context, index));
    break;
  case 'b': {
    // ---- タイムトラベル (Reverse Step / Reverse Continue) ----
    if (*cursor == 's') {
      // 'bs' = Reverse Single Step
      if (context != nullptr && thread_is_stopped(inspected) &&
          restore_previous_snapshot(inspected, context)) {
        stop_reply(SIGNAL_TRAP, inspected);
      } else {
        reply_text("E01");
      }
      break;
    }
    if (*cursor == 'c') {
      // 'bc' = Reverse Continue
      if (context != nullptr && thread_is_stopped(inspected) && g_snap_count > 0) {
        bool hit_bp = false;
        while (g_snap_count > 0) {
          restore_previous_snapshot(inspected, context);
          uint32_t cur_pc = read_register(context, 15);
          for (uint32_t b = 0; b < g_breakpoint_count; ++b) {
            if ((g_breakpoints[b] & ~1u) == (cur_pc & ~1u)) {
              hit_bp = true;
              break;
            }
          }
          if (hit_bp) break;
        }
        stop_reply(SIGNAL_TRAP, inspected);
      } else {
        reply_text("E01");
      }
      break;
    }
    break;
  }
  case 'P': {
    // ---- 単一レジスタ書き込み (P<reg_hex>=<val_hex>) ----
    const uint32_t index = parse_hex(cursor);
    while (*cursor != '=' && *cursor != ' ') ++cursor;
    if (*cursor == '=') ++cursor;
    const uint32_t val = parse_word_hex(cursor);
    if (context == nullptr || !thread_is_stopped(inspected) || index >= REGISTER_COUNT) {
      reply_text("E01");
    } else {
      if (write_register(context, index, val))
        reply_text("OK");
      else
        reply_text("E01");
    }
    break;
  }
  case 'G': {
    // ---- 全レジスタ一括書き込み ----
    if (context == nullptr || !thread_is_stopped(inspected)) {
      reply_text("E01");
    } else {
      bool ok = true;
      for (uint32_t i = 0; i < REGISTER_COUNT; ++i) {
        uint32_t val = parse_word_hex(cursor);
        if (!write_register(context, i, val))
          ok = false;
      }
      reply_text(ok ? "OK" : "E01");
    }
    break;
  }

  case 'p': {
    const uint32_t index = parse_hex(cursor);
    if (context == nullptr || !thread_is_stopped(inspected) ||
        index >= REGISTER_COUNT)
      reply_text("E01");
    else
      reply_word_hex(read_register(context, index));
    break;
  }
  case 'm': {
    const uintptr_t address = parse_hex(cursor);
    ++cursor; // ','
    const uint32_t bytes = parse_hex(cursor);
    if (!address_is_readable(address, bytes)) {
      reply_text("E01");
      break;
    }
    const uint8_t *source = (const uint8_t *)address;
    for (uint32_t index = 0; index < bytes; ++index)
      reply_byte_hex(source[index]);
    break;
  }
  case 'M': {
    const uintptr_t address = parse_hex(cursor);
    if (*cursor == ',') ++cursor;
    const uint32_t bytes = parse_hex(cursor);
    if (*cursor == ':') ++cursor;
    if (!address_is_writable(address, bytes)) {
      reply_text("E01");
      break;
    }
    uint8_t *dest = (uint8_t *)address;
    for (uint32_t index = 0; index < bytes; ++index) {
      int high = from_hex(*cursor++);
      int low = from_hex(*cursor++);
      if (high < 0 || low < 0) {
        reply_text("E01");
        break;
      }
      dest[index] = (uint8_t)((high << 4) | low);
    }
    reply_text("OK");
    break;
  }
  case 'X': {
    const uintptr_t address = parse_hex(cursor);
    if (*cursor == ',') ++cursor;
    const uint32_t bytes = parse_hex(cursor);
    if (*cursor == ':') ++cursor;
    if (!address_is_writable(address, bytes)) {
      reply_text("E01");
      break;
    }
    uint8_t *dest = (uint8_t *)address;
    for (uint32_t index = 0; index < bytes; ++index) {
      uint8_t byte = (uint8_t)*cursor++;
      if (byte == '}') {
        byte = (uint8_t)(*cursor++ ^ 0x20);
      }
      dest[index] = byte;
    }
    reply_text("OK");
    break;
  }
  case 'Z':
  case 'z': {
    const bool insert = g_packet[0] == 'Z';
    const char kind = *cursor++;
    ++cursor; // ','
    const uintptr_t address = parse_hex(cursor);
    // ★0 (ソフトウェア) も 1 (ハードウェア) も FPB で置く。**flash は書けない**
    //   ので BKPT を埋める道が無い — 数は FP_CTRL が持っている数まで。
    if (kind != '0' && kind != '1') {
      break; // 未対応 (ウォッチポイントは空返事)
    }
    if (insert) {
      if (g_breakpoint_count >= ARCH::breakpoint_count()) {
        reply_text("E01");
        break;
      }
      g_breakpoints[g_breakpoint_count++] = address;
    } else {
      for (uint32_t index = 0; index < g_breakpoint_count; ++index)
        if (g_breakpoints[index] == address) {
          g_breakpoints[index] = g_breakpoints[--g_breakpoint_count];
          break;
        }
    }
    // ★一覧を書き換えたら agent へ頼み、**仕掛かるまで待ってから** OK を返す
    //   (D49)。server 自身は FPB を触らない — 触れるのは自分のコアの分だけで、
    //   対象が別のコアに居ると意味が無いため。
    request_rearm();
    wait_for_agents();
    reply_text("OK");
    break;
  }
  case 'v':
    // ★GDB は繋いだ直後に vCont? を聞く。空返事だと「対応していない」と解釈され、
    //   継続とステップの状態管理が古い経路のままになる。実測でそれが原因で
    //   stepi が "Cannot execute this command while the target is running" に
    //   なった (止まっているのに GDB 側が走っていると思っていた)。
    // ★vStopped = 「さっきの通知を引き取った。次があれば寄こせ」(D53)。
    //   ここで初めて待ち行列から消す。次があればそれを返し、無ければ OK。
    //   OK を返した時点で「通知は未処理でない」状態に戻り、次の通知を
    //   送ってよくなる。
    if (__builtin_strncmp(cursor, "Stopped", 7) == 0) {
      // 直前に通知した分を消す (通知は先頭を見せるだけで消していない)。
      take_pending(true);
      if (take_pending(false)) {
        // まだある: そのまま返し、通知は未処理のまま (次も vStopped で来る)
        send_reply();
        return;
      }
      g_notify_outstanding = false;
      // ★★**組みかけを捨ててから** OK を書く。take_pending(true) が
      //   バッファへ組んだ停止応答が残っており、そこへ足すと
      //   `T05thread:4;OK` という 1 つの化けたパケットになる
      //   (実測で踏んだ: GDB が "Malformed packet (missing colon): OK" と言う)。
      reply_reset();
      reply_text("OK");
      break;
    }
    if (__builtin_strncmp(cursor, "Cont?", 5) == 0) {
      reply_text("vCont;c;C;s;S;t");
      break;
    }
    if (__builtin_strncmp(cursor, "Cont;", 5) == 0) {
      const char action = cursor[5];
      // ★★`vCont;c:3` のように**相手が付いてくる** (D51)。以前は捨てていたので
      //   GDB が「このスレッドを step しろ」と言っても既定の 1 本を動かして
      //   いた。付いていれば従い、無ければ H の選択 (無ければ既定) に倒す。
      const char *at = cursor + 6;
      // ★相手が明示されたか / それが掴める相手だったか、を分けて持つ (D55)。
      //   明示されたのに掴めない (存在しない・保護対象) 相手のとき、
      //   **既定へ倒して別のスレッドを動かしてはいけない** — 「9 番を止めろ」
      //   と言われて 4 番を止めるのは、頼まれていないことを黙ってやること
      //   (実測で踏んだ: 保護対象を狙ったら blink が止まった)。
      bool tid_given = false, tid_ok = false;
      if (*at == ':') {
        ++at;
        tid_given = true;
        if (*at == '-') {
          tid_ok = true; // -1 = 全部 = 既定でよい
        } else {
          const uint32_t id = parse_hex(at);
          if (id != 0 && thread_is_listable(from_gdb_id(id))) {
            g_selected_resume = from_gdb_id(id);
            tid_ok = true;
          }
        }
      }
      if (tid_given && !tid_ok) {
        // 掴めない相手を名指しされた。**何もしない**で OK を返す。
        reply_reset();
        reply_text("OK");
        break;
      }
      // ★★★`vCont;t` = 「この相手を止めろ」(D53)。**non-stop では GDB が
      //   これを使って個別に止める**ので、対応していないと接続時点で
      //   `Remote server does not support stopping threads` と言われて
      //   繋がらない (実測)。vCont? の返事にも 't' を載せること — 載せないと
      //   GDB は「使えない」と判断する。
      if (action == 't') {
        const uint32_t victim = resume_thread_id();
        if (!thread_is_protected(victim) && thread_is_alive(victim)) {
          kernel_instance.suspend(victim);
          mark_stopped(victim);
          // 頼まれて止めたので SIGINT で名乗る。
          queue_stop(victim, SIGNAL_INT);
          notify_if_needed();
        }
        reply_reset();
        reply_text("OK");
        break;
      }
      // ★★non-stop では**即 OK を返す** (D53)。all-stop のときだけ
      //   「止まるまで無言」(D48) を続ける — あちらは停止応答が
      //   continue への返事そのものなので。
      if (action == 's' || action == 'S') {
        do_step();
        if (g_nonstop) {
          reply_reset();
          reply_text("OK");
          break;
        }
        return;
      }
      do_continue();
      if (g_nonstop) {
        reply_reset();
        reply_text("OK");
        break;
      }
      return;
    }
    break;
  case 'c': {
    do_continue();
    return;
  }
  case 's': {
    do_step();
    return;
  }
  case 'D':
  case 'k':
    // ★空の一覧を仕掛け直させる = 全コアで外れる。ここでも server は
    //   自分では触らない (D49)。
    g_breakpoint_count = 0;
    request_rearm();
    wait_for_agents();
    resume_all_stopped(); // D52: 止めた全部を戻す (1 本だけでは足りない)
    g_attached = false;
    // ★停止応答の借りも消す。残すと、切った後に対象が偶然止まった瞬間、
    //   誰も居ない相手へ T05 を投げる (次に繋いだ客が 1 つずれた返事を受ける)。
    g_running = false;
    reply_text("OK");
    break;
  default:
    break; // 空返事 = 未対応
  }
  send_reply();
}

// ---- デバッグ対象 ----------------------------------------------------------
// ★止めて覗いて再開する相手。**普通のオブジェクトのスレッド**で、デバッグのために
//   特別なことは何もしていない (それが確かめたいこと)。
volatile uint32_t g_debuggee_rounds = 0;

[[gnu::noinline]] void debuggee_step() {
  g_debuggee_rounds = g_debuggee_rounds + 1;
}

uintptr_t debuggee_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::DECLARE_NAME, (uintptr_t) "debuggee");
  while (true) {
    debuggee_step();
    api(object_api::SLEEP_US, 100000); // 10Hz
  }
  return 0;
}

// ---- agent: 自コアのデバッグハードウェアだけを面倒見る (D49) ----------------
// ★**ここが唯一 FPB / DEMCR に触る場所**。コアごとに 1 本走る。
//   server から「一覧が変わった」と言われたら、自分のコアへ仕掛け直す。
//   ★この 1 本が特権を要る側 — 逆に言えば、RSP を解釈する server から
//     特権を外せる (CDC がストリームになれば。D42 残)。
uintptr_t __not_in_flash_func(agent_main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::DECLARE_NAME, (uintptr_t) "gdbagent");
  const uint32_t core = BOARD::core_num();
  // ★MON_EN は**このコアの分**。両コアで立てないと、反対のコアで走る対象は
  //   DebugMonitor に入らない。
  ARCH::debug_enable(true);
  // ★立ったかを読んで確かめる (立てた、ではなく)。プローブが繋がっていると
  //   C_DEBUGEN が勝って MON_EN は立たない (D40 の正直な制約)。
  if (!ARCH::debug_enabled())
    BOARD::diag_printf("[GDB] core %lu: MON_EN did not take (a probe attached?)\n",
                       (unsigned long)core);
  while (true) {
    uint32_t current =
        ARCH::load_acquire32((volatile uint32_t *)&g_agent_alive);
    if (ARCH::cas32((volatile uint32_t *)&g_agent_alive, current,
                    current | (1u << core)))
      break;
  }

  uint32_t applied = 0xFFFFFFFFu; // 最初は必ず 1 回仕掛ける
  while (true) {
    const uint32_t want =
        ARCH::load_acquire32((volatile uint32_t *)&g_arm_generation);
    if (want != applied) {
      agent_apply_breakpoints();
      applied = want;
      ARCH::store_release32((volatile uint32_t *)&g_agent_applied[core], want);
    }
    // ★繋がれていない間は**寝る** (D48 と同じ理由)。繋がれている間だけ
    //   細かく見る — Z パケットへの返事を待たせるのはここの間隔なので。
    api(object_api::SLEEP_US,
        g_attached ? AGENT_BUSY_SLEEP_US : GDB_IDLE_SLEEP_US);
  }
  return 0;
}

uintptr_t stub_main(uintptr_t target, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::DECLARE_NAME, (uintptr_t) "gdbserver");
  g_target_thread = (uint32_t)target;
  g_self_thread = kernel_instance.current_thread_id();
  // ★席に座るのはこのスレッド (席は発行元のオブジェクトから導出されるので、
  //   実際に読み書きする側で座らないと意味がない)。
  if (g_link_active) {
    api(object_api::STREAM_BIND, g_link_in_id,
        (uintptr_t)stream::role::CONSUMER);
    api(object_api::STREAM_BIND, g_link_out_id,
        (uintptr_t)stream::role::PRODUCER);
  }
  // ★★DEMCR / FPB はここでは触らない (D49)。触れるのは自分のコアの分だけで、
  //   対象が別のコアに居ると意味が無い — 面倒を見るのは各コアの agent。
  uint32_t idle_rounds = 0;
  while (true) {
    // ★パケットを 1 つ受け取ってから判断する。以前は「'$' を 1 文字覗いて
    //   アタッチとみなす」細工を入れていたが、その 1 文字を食べたせいで応答が
    //   1 つずれ、GDB が vMustReplyEmpty への返事として qSupported の答えを
    //   受け取っていた (Remote replied unexpectedly と言われた)。覗かない。
    // ★診断を黙らせる仕掛けは**消した**。CDC が分かれた今、混ざりようが無い
    //   (D42)。以前は 1 バイト来た時点で黙らせていたが、それは 1 本を共有して
    //   いたための応急処置だった。
    // ★★誰も繋いでいない間は**寝る** (D48)。以前はここも YIELD で回り続けて
    //   いたので、デバッグしていない間ずっと (= 稼働時間のほぼ全部)、
    //   スケジューラを 1 周するたびに stub の syscall が 1 つ挟まっていた。
    //   繋がっていないなら仕事は無いので、寝て構わない。
    //   ★YIELD ではなく SLEEP_US なのが肝: YIELD は「走れる」ままなので
    //     スケジューラは毎周こちらを起こしに来る。寝れば起こしに来ない。
    //   ★対象は止めない — 繋がれていないのだから、対象は普通に走るのが正しい。
    if (!link_connected()) {
      if (g_attached) {
        // 繋がっていたのに切られた。★**自分が止めたものを全部**走らせて
        //   元に戻す (D52)。1 本だけ戻すと、複数止めていたとき残りが
        //   止まったままになる。
        g_attached = false;
        g_running = false;
        resume_all_stopped();
        BOARD::diag_printf("[GDB] client disconnected, the target is running again\n");
      }
      api(object_api::SLEEP_US, GDB_IDLE_SLEEP_US);
      continue;
    }

    // ★走らせている最中でも、ここへ必ず戻ってくる (D48)。だから
    //   「止まったか」を毎周見られるし、GDB の 0x03 も拾える。
    if (poll_for_stop())
      continue;
    // ★溜まっていて未通知のものがあれば出す (D53)。vStopped で引き取られて
    //   「もう無い」と答えた後に次が溜まった場合、ここが送り口になる。
    notify_if_needed();

    const int first = read_byte();

    // ★★裸の 0x03 = 「止めろ」(Ctrl-C / VS Code の一時停止ボタン)。パケットでは
    //   ないので '$' から始まらない — receive_packet へ渡す前にここで捌く。
    //   走らせていないときに来ることもある (GDB が念のため送る) が、その場合も
    //   停止応答を返しておく: 既に止まっているので嘘にはならず、
    //   応答を待っている相手を放置しない。
    if (first == 0x03) {
      interrupt_target();
      idle_rounds = 0;
      continue;
    }

    if (!receive_packet(first)) {
      // 接続が生きている間は、ブレークポイント停止中に勝手に再開させない。
      // (切断検知は link_connected() が行う)
      api(object_api::YIELD);
      continue;
    }
    idle_rounds = 0;
    if (!g_attached) {
      g_attached = true;
      BOARD::diag_printf("[GDB] a client attached on the GDB channel\n");
      // 繋いできた側は「止まっている」ことを期待している。
      if (!thread_is_protected(g_target_thread)) {
        kernel_instance.suspend(g_target_thread);
        mark_stopped(g_target_thread);
      }
    }
    handle_packet();

  }
  return 0;
}

} // namespace

uint32_t start_gdb_stub(uint32_t external_target) {
  uint32_t target_thread = external_target;
  if (target_thread == NO_EXTERNAL_TARGET) {
    // 覗かれる側を先に起こす (既定: 合成の debuggee)。
    api(object_api::CREATE_OBJECT, DEBUGGEE_OBJECT, (uintptr_t)&debuggee_main, 0);
    const auto debuggee = api(object_api::SPAWN, DEBUGGEE_OBJECT, 0, 0);
    if (debuggee.error != 0)
      return 1;
    target_thread = (uint32_t)debuggee.value;
  }

  // ★★agent を**コアごとに 1 本ずつ**起こす (D49)。FPB も DEMCR もコアごとに
  //   独立していて、書けるのは自分が走っているコアの分だけ — だから
  //   「コアの数だけ、それぞれのコアに固定して」置く。これで対象が
  //   どちらのコアで走っていても止まる。
  //   ★★★これが**特権を要る唯一の側**。RSP を解釈する server (下) から
  //     デバッグハードウェアを取り上げられるのが、分けたことの本当の見返り。
  if (api(object_api::CREATE_OBJECT, GDB_AGENT_OBJECT, (uintptr_t)&agent_main,
          OBJECT_PRIVILEGED)
          .error != 0)
    return 1;
  for (uint32_t core = 0; core < KERNEL::CORE_COUNT; ++core) {
    // ★1 本ずつ**別のコアへ固定する**。固定しないと 2 本とも同じコアに
    //   乗りかねず、反対のコアには誰も居ないという元の壊れ方に戻る。
    //   ★宣言を書き換えてから spawn する (アフィニティは spawn の時点で
    //     読まれるので、後から変えるより競争が無い)。
    api(object_api::SET_OBJECT_AFFINITY, GDB_AGENT_OBJECT, 1u << core);
    const auto agent = api(object_api::SPAWN, GDB_AGENT_OBJECT, 0, 0);
    // ★★**spawn した側で登録する** (D55)。agent 自身に名乗らせると、
    //   起き上がるまでの間だけ一覧に出て止められる窓ができる。ここなら
    //   走り出す前に閉じられる。
    g_protected_threads |= (1u << (uint32_t)agent.value);
    kernel_instance.set_thread_debug_protected(agent.value, true);
    if (agent.error != 0) {
      BOARD::diag_printf("[GDB] could not spawn the agent for core %lu\n",
                         (unsigned long)core);
      return 1;
    }
  }

  // ★server は**どのコアで走ってもよい** — デバッグハードウェアに触らないので、
  //   自分がどこに居るかは関係ない (D49)。
  const auto created = api(object_api::CREATE_OBJECT, GDB_SERVER_OBJECT,
                           (uintptr_t)&stub_main, 0);
  if (created.error != 0)
    return 1;
  const auto spawned =
      api(object_api::SPAWN, GDB_SERVER_OBJECT, 0, target_thread);
  g_protected_threads |= (1u << (uint32_t)spawned.value); // D55
  kernel_instance.set_thread_debug_protected(spawned.value, true);
  if (spawned.error != 0)
    return 1;
  if (g_link_active) {
    BOARD::diag_printf(
        "[GDB] server ready **over a stream** (in %lu / out %lu).\n"
        "[GDB] 転送側が pump して、繋がったら gdb_link_set_connected(true)。\n"
        "[GDB] watching thread %lu, %lu hardware breakpoints, %lu core agents\n",
        (unsigned long)g_link_in_id, (unsigned long)g_link_out_id,
        (unsigned long)target_thread, (unsigned long)ARCH::breakpoint_count(),
        (unsigned long)KERNEL::CORE_COUNT);
  } else {
    BOARD::diag_printf(
        "[GDB] server ready on this port. attach with:\n"
        "[GDB]   arm-none-eabi-gdb bazel-bin/firmware/shizuku\n"
        "[GDB]   (gdb) target remote /dev/cu.usbmodem*\n"
        "[GDB] watching thread %lu, %lu hardware breakpoints, %lu core agents\n",
        (unsigned long)target_thread, (unsigned long)ARCH::breakpoint_count(),
        (unsigned long)KERNEL::CORE_COUNT);
  }
  return 0;
}

gdb_link start_gdb_stub_over_stream(uint32_t external_target) {
  gdb_link link{NO_LINK, NO_LINK, false};
  g_link_in.init();
  g_link_out.init();
  const auto in_created =
      api(object_api::STREAM_CREATE, (uintptr_t)&g_link_in.desc);
  const auto out_created =
      api(object_api::STREAM_CREATE, (uintptr_t)&g_link_out.desc);
  if (in_created.error != 0 || out_created.error != 0) {
    BOARD::diag_printf("[GDB] could not create the link streams (%lu/%lu)\n",
                       (unsigned long)in_created.error,
                       (unsigned long)out_created.error);
    return link;
  }
  g_link_in_id = in_created.value;
  g_link_out_id = out_created.value;
  // ★ここを立ててから start する — stub_main が席に座るかどうかを見るため。
  g_link_active = true;
  if (start_gdb_stub(external_target) != 0) {
    g_link_active = false;
    return link;
  }
  link.to_stub = g_link_in_id;
  link.from_stub = g_link_out_id;
  link.ok = true;
  return link;
}

void gdb_link_set_connected(bool connected) {
  g_link_connected = connected;
}

void gdb_link_protect_thread(uint32_t thread) {
  if (thread < 32)
    g_protected_threads |= (1u << thread);
    kernel_instance.set_thread_debug_protected(thread, true);
}

} // namespace objects
} // namespace shizuku
// Test comment
