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
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/gdb_stub.hpp"

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
bool g_quiet = false; // 診断を黙らせているか
uint32_t g_target_thread = 0;
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
int read_byte() { return ::getchar_timeout_us(0); }
void write_byte(char value) { ::putchar_raw(value); }
// ★★書いたら**必ず押し出す**。pico の stdio は改行かバッファ満杯まで溜め込むが、
//   GDB の返事に改行は無い。溜め込まれると返事が遅れて届き、GDB から見ると
//   「1 つ前の問いへの答え」が来たように見える
//   (実際に "Remote replied unexpectedly to 'vMustReplyEmpty'" になった)。
void flush_out() { ::stdio_flush(); }

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
void send_reply() {
  uint8_t sum = 0;
  write_byte('$');
  for (uint32_t index = 0; index < g_reply_length; ++index) {
    write_byte(g_reply[index]);
    sum = (uint8_t)(sum + (uint8_t)g_reply[index]);
  }
  write_byte('#');
  write_byte(to_hex(sum >> 4));
  write_byte(to_hex(sum & 0xF));
  flush_out();
}

// 1 パケット受け取る。★**どの待ちにも上限を付ける**。上限が無いと、途中で切れた
//   入力ひとつで stub が回り続け、返事もしないまま診断まで道連れに黙る
//   (実際に踏んだ: ログが止まり、GDB にも probe にも何も返らなくなった)。
//   チェックサムが合わなければ '-' を返して再送させる — 化けた要求は実行しない。
constexpr uint32_t READ_PATIENCE = 30000; // 空振りをこれだけ数えたら諦める

int read_byte_waiting() {
  for (uint32_t idle = 0; idle < READ_PATIENCE; ++idle) {
    const int value = read_byte();
    if (value >= 0)
      return value;
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

// ---- 要求ごとの処理 --------------------------------------------------------
uint32_t parse_hex(const char *&cursor) {
  uint32_t value = 0;
  while (from_hex(*cursor) >= 0) {
    value = (value << 4) | (uint32_t)from_hex(*cursor);
    ++cursor;
  }
  return value;
}

void arm_breakpoints() {
  ARCH::breakpoint_enable(true);
  for (uint32_t index = 0; index < g_breakpoint_count; ++index)
    ARCH::breakpoint_set(index, g_breakpoints[index]);
}

void stop_reply() {
  reply_reset();
  reply_text("T05thread:1;"); // SIGTRAP、スレッド 1 で停止
  send_reply();
}

// continue / step のあと、止まるまで待つ。★待っている間も**譲る** — 待ちで
//   他のスレッドを止めたら、この設計の売りが消える。
// ★上限は**短く**する。長いと、止まらない相手を待っている間に stub が戻らず、
//   診断も黙ったまま、picotool のリセット要求にも応えられなくなる
//   (実際にそれで焼けなくなり、1200bps タッチで BOOTSEL へ落として復旧した)。
//   待ちきれなかったら「止まった」ことにして GDB へ返す — 嘘をつくより、
//   応答を返して相手に判断させるほうが復旧できる。
void wait_for_stop(uint32_t since) {
  for (uint32_t guard = 0; guard < 100000; ++guard) {
    if (kernel_instance.debug_event_count() != since)
      return;
    api(object_api::YIELD);
  }
}

void do_continue() {
  const uint32_t since = kernel_instance.debug_event_count();
  kernel_instance.resume(g_target_thread);
  wait_for_stop(since);
  g_target_thread = kernel_instance.debug_event().thread;
  stop_reply();
}

void do_step() {
  const uint32_t since = kernel_instance.debug_event_count();
  // ★MON_STEP は**コア単位**でスレッド単位ではない。狙った相手が次に走るとは
  //   限らないので、違う相手が止まったら追い直す。
  for (uint32_t attempt = 0; attempt < 32; ++attempt) {
    ARCH::debug_step(true);
    kernel_instance.resume(g_target_thread);
    wait_for_stop(since);
    if (kernel_instance.debug_event().thread == g_target_thread)
      break;
    kernel_instance.resume(kernel_instance.debug_event().thread);
  }
  stop_reply();
}

void handle_packet() {
  const char *cursor = g_packet;
  KERNEL::CONTEXT *context = kernel_instance.thread_context(g_target_thread);
  reply_reset();
  switch (*cursor++) {
  case 'q':
    if (__builtin_strncmp(cursor, "Supported", 9) == 0)
      reply_text("PacketSize=3ff");
    else if (__builtin_strncmp(cursor, "Attached", 8) == 0)
      reply_text("1");
    else if (*cursor == 'C')
      reply_text("QC1");
    // ★スレッドの一覧を返さないと、GDB は「走っているスレッドが無い」と判断して
    //   [Thread 1 exited] と言って何もできなくなる (実際にそうなった)。
    //   1 本だけ見せる: f = 最初の一括、s = 続き (l = これで終わり)。
    else if (__builtin_strncmp(cursor, "fThreadInfo", 11) == 0)
      reply_text("m1");
    else if (__builtin_strncmp(cursor, "sThreadInfo", 11) == 0)
      reply_text("l");
    break;
  case 'T': // そのスレッドは生きているか
    reply_text("OK");
    break;
  case '?':
    // ★どのスレッドで止まっているかまで言う。S05 だけだと GDB がスレッドを
    //   結び付けられない。
    reply_text("T05thread:1;");
    break;
  case 'H': // スレッドの選択。1 本しか見せていないので常に OK
  case 'Q':
    reply_text("OK");
    break;
  case 'g':
    if (context != nullptr)
      for (uint32_t index = 0; index < REGISTER_COUNT; ++index)
        reply_word_hex(read_register(context, index));
    break;
  case 'p': {
    const uint32_t index = parse_hex(cursor);
    if (context != nullptr && index < REGISTER_COUNT)
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
      arm_breakpoints();
    } else {
      for (uint32_t index = 0; index < g_breakpoint_count; ++index)
        if (g_breakpoints[index] == address) {
          g_breakpoints[index] = g_breakpoints[--g_breakpoint_count];
          break;
        }
      for (uint32_t index = 0; index < ARCH::breakpoint_count(); ++index)
        ARCH::breakpoint_clear(index);
      arm_breakpoints();
    }
    reply_text("OK");
    break;
  }
  case 'v':
    // ★GDB は繋いだ直後に vCont? を聞く。空返事だと「対応していない」と解釈され、
    //   継続とステップの状態管理が古い経路のままになる。実測でそれが原因で
    //   stepi が "Cannot execute this command while the target is running" に
    //   なった (止まっているのに GDB 側が走っていると思っていた)。
    if (__builtin_strncmp(cursor, "Cont?", 5) == 0) {
      reply_text("vCont;c;C;s;S");
      break;
    }
    if (__builtin_strncmp(cursor, "Cont;", 5) == 0) {
      const char action = cursor[5];
      if (action == 's' || action == 'S') {
        do_step();
        return;
      }
      do_continue();
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
    for (uint32_t index = 0; index < ARCH::breakpoint_count(); ++index)
      ARCH::breakpoint_clear(index);
    g_breakpoint_count = 0;
    kernel_instance.resume(g_target_thread);
    g_attached = false;
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

uintptr_t stub_main(uintptr_t target, uintptr_t, uintptr_t, uintptr_t) {
  api(object_api::DECLARE_NAME, (uintptr_t) "gdbstub");
  g_target_thread = (uint32_t)target;
  ARCH::debug_enable(true);
  uint32_t idle_rounds = 0;
  while (true) {
    // ★パケットを 1 つ受け取ってから判断する。以前は「'$' を 1 文字覗いて
    //   アタッチとみなす」細工を入れていたが、その 1 文字を食べたせいで応答が
    //   1 つずれ、GDB が vMustReplyEmpty への返事として qSupported の答えを
    //   受け取っていた (Remote replied unexpectedly と言われた)。覗かない。
    const int first = read_byte();
    // ★★**1 バイトでも来たら即黙る**。正しいパケットを受け取ってから黙らせると、
    //   その間に出た診断の文字が GDB の受信の先頭に混ざり、握手が壊れる
    //   (実測: "Remote replied unexpectedly to 'vMustReplyEmpty'" になる。
    //   RSP 自体は正しく、汚れていたのは通り道のほうだった)。
    //   ★これは応急処置で、本筋は CDC をもう 1 本に分けること (D41)。
    if (first >= 0 && !g_quiet) {
      g_quiet = true;
      BOARD::diag_mute(true);
    }
    if (!receive_packet(first)) {
      // ★繋がっていたのに黙ったままなら、相手は居なくなったとみなして戻す。
      //   D を送らずに切る相手 (ケーブルを抜く等) がいるので、自分で復帰する。
      if (g_quiet && ++idle_rounds > 20000) {
        g_attached = false;
        g_quiet = false;
        idle_rounds = 0;
        BOARD::diag_mute(false);
        BOARD::diag_printf("[GDB] client went away, diagnostics are back\n");
      }
      api(object_api::YIELD);
      continue;
    }
    idle_rounds = 0;
    if (!g_attached) {
      // ★ここから CDC は GDB のもの — 人間向けの文字を混ぜると相手が壊れる。
      //   ★黙らせるのは**正しいパケットを受け取ってから**。'$' を見た時点で
      //     黙らせると、ゴミ 1 文字で診断が止まったまま戻らなくなる。
      g_attached = true;
      BOARD::diag_mute(true);
      // 繋いできた側は「止まっている」ことを期待している。
      kernel_instance.suspend(g_target_thread);
    }
    handle_packet();
    if (!g_attached && g_quiet) {
      g_quiet = false;
      BOARD::diag_mute(false);
    }
  }
  return 0;
}

} // namespace

uint32_t start_gdb_stub() {
  // 覗かれる側を先に起こす。
  api(object_api::CREATE_OBJECT, DEBUGGEE_OBJECT, (uintptr_t)&debuggee_main, 0);
  const auto debuggee = api(object_api::SPAWN, DEBUGGEE_OBJECT, 0, 0);
  if (debuggee.error != 0)
    return 1;
  const uint32_t target_thread = (uint32_t)debuggee.value;

  const auto created = api(object_api::CREATE_OBJECT, GDB_STUB_OBJECT,
                           (uintptr_t)&stub_main, 0);
  if (created.error != 0)
    return 1;
  const auto spawned =
      api(object_api::SPAWN, GDB_STUB_OBJECT, 0, target_thread);
  if (spawned.error != 0)
    return 1;
  BOARD::diag_printf(
      "[GDB] stub ready on this port. attach with:\n"
      "[GDB]   arm-none-eabi-gdb bazel-bin/firmware/shizuku\n"
      "[GDB]   (gdb) target remote /dev/cu.usbmodem*\n"
      "[GDB] watching thread %lu ('debuggee'), %lu hardware breakpoints\n"
      "[GDB] try: (gdb) break debuggee_step / continue / info registers\n",
      (unsigned long)target_thread, (unsigned long)ARCH::breakpoint_count());
  return 0;
}

} // namespace objects
} // namespace shizuku
