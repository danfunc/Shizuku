#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/exception.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/boards/rp2350_pico2.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/objects/usb_cdc.hpp"
#include "tusb.h"

// board は「どの ISA の上に居るか」を知っている層でもある (region を張る機構は
// arch 側)。ここだけの別名にして、上位のテンプレート引数とは混ぜない。
using ARCH_TYPE = shizuku::archs::armv8m;

// ★GDB が同じ CDC を使っている間は診断を黙らせる (board.cpp の diag_printf)。
//   CDC をストリーム化すれば、この旗ごと要らなくなる。
static bool g_diag_quiet = false;

// リンカが置く「静的データの終端 = ヒープの先頭」。名前空間の中で宣言すると
// 名前が飾られて別物になるので、必ずファイル先頭で C リンケージとして宣言する。
extern "C" char __end__[];
// リンカが置く「ファーム本体の終端」。flash_fs.cpp と同じ名前を同じ理由で
// ファイルスコープの extern "C" として受ける。
extern "C" char __flash_binary_end;
#include <cstdarg>
#include <cstdio>

// USB の割り込みハンドラ。フォールト報告を「割り込みが走れない状況でも」外へ
// 出すために、ポーリングで呼べる形で握っておく (board::init が拾う)。
static void (*g_usb_irq_poll)() = nullptr;

namespace shizuku {
namespace boards {

void rp2350_pico2::init(uint32_t core) {
  if (core == 0) {
    // RAM ベクタテーブルは両コア共有: 登録は core0 の 1 回だけ
    // (exclusive 登録は二重登録で panic するため、core1 で再登録しない)。
    exception_set_exclusive_handler(SVCALL_EXCEPTION, shizuku_armv8m_svc_entry);
    exception_set_exclusive_handler(PENDSV_EXCEPTION,
                                    shizuku_armv8m_pendsv_entry);
    exception_set_exclusive_handler(SYSTICK_EXCEPTION,
                                    shizuku_armv8m_systick_entry);
    // ★無言で固まらせないための最後の砦。ここを用意していなかったせいで、
    //   スタック枯渇 → HardFault → USB ごと停止 → 書き込みもできない、という
    //   一番情報の少ない壊れ方をした (PORT §7 が最初に用意しろと書いている項目)。
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    // ★スタック下限違反や MPU 違反は本来「設定可能な優先度を持つ例外」なので、
    //   有効にしておけば HardFault へ落ちない。HardFault は優先度 -1 で**あらゆる
    //   割り込みを止める**ため、そこから USB へ何かを出すことが構造的にできない。
    //   有効化して優先度を USB より下に置けば、報告中も USB 割り込みが走れる。
    exception_set_exclusive_handler(MEMMANAGE_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    exception_set_exclusive_handler(BUSFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    exception_set_exclusive_handler(USAGEFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
  }
  // ★このコアを「止められる側」として仕込む。FIFO 割り込みのハンドラは RAM に
  //   置かれており、止められている間 flash を 1 バイトも読まない。
  //   ★★**両方のコアで呼ぶ** (2026-08-24 修正)。以前は `core != 0` で core1
  //     だけ登録していたが、`flash_safe_execute()` が見るのは「**自分ではない
  //     方**のコアが victim 登録済みか」(pico_flash/flash.c:184)。core0 を
  //     登録しないと、**core1 から flash を書けない** —
  //     `assert(false)` (flash.c:190) で止まる。実際 OTA の受信スレッドが
  //     core1 に乗って踏んだ (`[PANIC] core=1 thread=8`)。bonding の書き込みが
  //     動いていたのは、それが core0 の BLE スレッドで走っていたからにすぎない。
  //   ★どちらのコアが書いても、書いている間もう片方は止まる (数十ms/セクタ)。
  //     それは flash 書き込みの性質そのものなので、コアの選び方では消せない。
  ::multicore_lockout_victim_init();
  // ★DebugMonitor (例外番号 12) は pico-sdk の exception_number に無いので、
  //   RAM ベクタへ直に据える。ベクタは両コア共有なので登録は core0 の 1 回だけ。
  if (core == 0) {
    ((void (**)())scb_hw->vtor)[12] = shizuku_armv8m_debugmon_entry;
  }
  // 優先度は banked なので各コアで設定する。SVC 最優先 = プリミティブの原子性、
  // PendSV 最低 = 全 IRQ が捌けてからのスレッド切替 (DESIGN §14.5.1)。
  // ★この順序が 1 コア内の相互排除を無償で与える (DESIGN §14.5.1)。
  //   syscall 最優先 → その中ではタイマにも切替にも割り込まれない。
  //   切替は最低優先度なので、ハンドラを抜けるまで走らない。
  exception_set_priority(SVCALL_EXCEPTION, 0x00);
  exception_set_priority(SYSTICK_EXCEPTION, 0x40);
  exception_set_priority(PENDSV_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);
  protection_init(); // region は per-core banked なので各コアで張る

  // ---- 落ちたときに必ず声が出るようにする -------------------------------
  // ★診断が届くかどうかは優先度で決まる。報告している最中に USB の割り込みが
  //   走れなければ、書いた文字はデバイスの中に留まったまま出て行かない。
  //   そこで USB を上げ (0x20)、フォールト例外はそれより下 (0x80) に置く。
  //   こうすると報告中も USB が動けるので、**落ちた事実と場所が必ず外へ出る**。
  irq_set_priority(USBCTRL_IRQ, 0x20);
  // 落ちたときに自分で回せるよう、USB の割り込みハンドラを控えておく。
  g_usb_irq_poll = (void (*)())irq_get_exclusive_handler(USBCTRL_IRQ);
  // ★DebugMon はフォールトより**下**に置く。デバッグ中に落ちたら、デバッグより
  //   先に落ちたことを報告できないと原因が分からなくなる。
  //   優先度はシステムハンドラ優先度レジスタ (SHPR) の 12 番目。
  ((volatile uint8_t *)(uintptr_t)0xE000ED18u)[12 - 4] = 0xA0;
  exception_set_priority(MEMMANAGE_EXCEPTION, 0x80);
  exception_set_priority(BUSFAULT_EXCEPTION, 0x80);
  exception_set_priority(USAGEFAULT_EXCEPTION, 0x80);
  // 設定可能なフォールトを有効化する (無効のままだと全部 HardFault へ落ちて
  // 優先度 -1 になり、上の工夫が効かなくなる)。SHCSR の該当ビット。
  scb_hw->shcsr |= (1u << 16) | (1u << 17) | (1u << 18); // MEM/BUS/USG FAULTENA
}

// ★どこに何を張るかは board の知識 (SoC のメモリ地図を知っているのはここだけ)。
//   狙いは 2 つ:
//     (i)  データを実行させない / コードを書き換えさせない (W^X)
//     (ii) region の外を特権だけに閉じる (PRIVDEFENA) — 単一アドレス空間で
//          「カーネル空間」を作る唯一の方法 (DESIGN §11.1)
//   ★静的データ (.bss/.data) は region の外に落ちる。つまり**非特権オブジェクトは
//     自分のグローバルに触れない**。これは事故ではなく設計で、オブジェクトの状態は
//     ヒープ上 (= region1) に置けという圧力になる (§11.2.2)。
void rp2350_pico2::protection_init() {
  // region0: **ファーム本体の範囲だけ** = 読み+実行のみ。
  // ★★以前は XIP 全域を張っていたので、非特権オブジェクトが flash_fs の
  //   データ領域 (ファーム末尾〜flash 終端) もポインタ計算だけで読めた
  //   (docs/03_porting_policy.md Q8)。ファームのコードは誰が実行するにも
  //   読めて当然だが、ファイルの中身は**それを開いた者だけ**が読めるべきなので、
  //   region0 はコード範囲だけに絞り、ファイルの中身は
  //   GRANT_REGION (§11.3 / Q8) で対象オブジェクトへ動的に開く。
  const uintptr_t code_end =
      ((uintptr_t)&__flash_binary_end + 31u) & ~(uintptr_t)31;
  ARCH_TYPE::region_set(0, XIP_BASE, code_end, ARCH_TYPE::ACCESS_RO_ALL,
                        false, 0);
  // region1: ヒープ先頭〜SRAM 終端 = 読み書き可・実行不可。スレッドスタックは
  // ここに住むので、スタック上のデータを実行する事故が止まる。
  const uintptr_t heap = ((uintptr_t)__end__ + 31u) & ~(uintptr_t)31;
  ARCH_TYPE::region_set(1, heap, SRAM_END - 32u, ARCH_TYPE::ACCESS_RW_ALL, true,
                        1);
  for (uint32_t index = 2; index < 8; ++index)
    ARCH_TYPE::region_disable(index);
  ARCH_TYPE::protection_enable();
}

uintptr_t rp2350_pico2::unprivileged_floor() {
  return ((uintptr_t)__end__ + 31u) & ~(uintptr_t)31; // region1 の先頭と同じ
}

uint32_t rp2350_pico2::cycles_per_us() {
  // 実クロックから毎回引く (クロックを変えても追従する。PORT §2.3)。
  return (uint32_t)(::clock_get_hz(clk_sys) / 1000000u);
}

void rp2350_pico2::diag_printf(const char *format, ...) {
  // ★GDB が同じ CDC を使っている間は黙る。人間向けの文字を混ぜると相手の
  //   プロトコルが壊れる (CDC をストリーム化すれば、この旗は消える)。
  if (g_diag_quiet)
    return;
  va_list args;
  va_start(args, format);
  ::vprintf(format, args);
  va_end(args);
}

void rp2350_pico2::panic(const char *message) {
  ::panic("%s", message);
}

// ★診断は「溢れたら捨てる」方針 (D42, usb_cdc.cpp) — 通常運転ではそれでよい
//   (本業を診断で止めない)。だが panic は**まとまった量を一度だけ**出す一回性の
//   イベントで、行間に間を置いても実害が無い。詰めて出すと CDC のソフトウェア
//   バッファを溢れさせて先頭の行から捨てられる (実測で踏んだ: [PANIC] の
//   ヘッダ行とメッセージが消え、後半のバックトレースだけ残った)。1 行ごとに
//   吐き出させてから次を書く。
void panic_diag_drain() {
  objects::usb_cdc_flush(0);
  ::sleep_ms(2);
}

// ★assert の本文を**必ず最後まで出す**。newlib の既定 __assert_func は
//   fiprintf で一気に書いてから abort するので、CDC が満杯だと途中で捨てられ
//   (diag_out_chars は「溢れたら捨てる」設計)、`assertion "false" failed:
//   file "exter` のようにファイル名の途中で切れる — これで実機の原因究明が
//   二度止まった (2026-08-24)。1 項目ずつ吐き出させてから次を書く。
extern "C" [[noreturn]] void shizuku_panic_dump(const char *fmt, ...);

extern "C" [[noreturn]] void __assert_func(const char *file, int line,
                                           const char *func,
                                           const char *expression) {
  panic_diag_drain();
  rp2350_pico2::diag_printf("\n[ASSERT] %s\n",
                            expression ? expression : "(no expression)");
  panic_diag_drain();
  rp2350_pico2::diag_printf("[ASSERT] file %s\n", file ? file : "(no file)");
  panic_diag_drain();
  rp2350_pico2::diag_printf("[ASSERT] line %d\n", line);
  panic_diag_drain();
  rp2350_pico2::diag_printf("[ASSERT] func %s\n", func ? func : "(no func)");
  panic_diag_drain();
  shizuku_panic_dump("assertion failed");
}

// ★configs/pico_config_extra_headers.h.in の PICO_PANIC_FUNCTION から呼ばれる。
//   既定の panic() (pico_sdk の pico_platform_panic/panic.c) はメッセージを
//   出すだけで bkpt #0 に落ち、呼び出し元も系の状態も残らない。ここでは
//   「誰が (コア・スレッド)」「スタックに積まれた戻り先の並び (簡易
//   バックトレース)」まで診断へ残してから、自前で割り込みを止めて完全に
//   停止する。
//   ★★★既定 (stdio_init_all を呼んだ構成) は puts では死なずに後段の
//   vprintf だけ固まることがある — pico-sdk 自身が panic.c に書いている通り
//   print_mutex が未初期化だと起き得る罠で、stdio_init_all を呼ばずに
//   usb_cdc_init だけで組んでいるこのリポジトリでは尚更ありうる。
//   PICO_PANIC_FUNCTION を定義した時点で panic.c はその経路
//   (puts/vprintf を含む #else 分岐) を通らなくなるので、ここでは既に
//   踏んでいない。diag_printf はこの起動全体で使い続けている経路をそのまま
//   使うので、同じ罠を新たに踏むこともない。
//   ★止め方は panic() の既定 (bkpt #0) に**乗らない**。DEMCR.MON_EN は
//   GDB stub が起動時に立てっぱなしなので (gdb_stub.cpp)、bkpt は
//   デバッガが繋がっていなくても DebugMonitor に捕まる — すると
//   「そのスレッドだけ止めて他は継続する」という通常の debug_dispatch の
//   挙動になってしまい、panic の「系ごと止める」という意味が壊れる。
//   ここでは割り込みそのものを cpsi で落としてから無限ループするので、
//   DebugMonitor の入口にすら来ない。
extern "C" [[noreturn]] void shizuku_panic_dump(const char *fmt, ...) {
  // ★プロローグで潰れる前に、できるだけ早く SP を拾っておく。panic() 自身は
  //   `push {lr}; bl shizuku_panic_dump` という素の asm なので、押した lr
  //   (panic() を呼んだ側の戻り先) は __builtin_return_address では取れない
  //   — スタック上を直接探すしかない (下のバックトレース走査で拾う)。
  void *entry_sp;
  asm volatile("mov %0, sp" : "=r"(entry_sp));

  const uint32_t core = ::get_core_num();
  // ★kernel_instance がまだ組み立てられていない起動の最初期でも呼ばれ得る。
  //   current_thread_id() は配列参照をしない (m_current[core] を読むだけ) ので、
  //   その段階で呼んでも安全。
  const uint32_t thread = kernel_instance.current_thread_id();

  rp2350_pico2::diag_printf("\n[PANIC] core=%lu thread=%lu\n",
                            (unsigned long)core, (unsigned long)thread);
  panic_diag_drain();
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    ::vprintf(fmt, args);
    va_end(args);
  }
  rp2350_pico2::diag_printf("\n");
  panic_diag_drain();

  // ★簡易バックトレース。フレームポインタは維持していない (Thumb の既定) ので
  //   厳密な巻き戻しはできないが、通り抜けてきた bl の戻り先はスタック上に
  //   (Thumb なので bit0=1 で) 転がっている。フラッシュのコード範囲に見える
  //   語だけを「たぶんここを通った」として拾う — 判定を厳しくして見落とすより、
  //   多めに出して addr2line にかけてもらう方を選ぶ。
  rp2350_pico2::diag_printf(
      "[PANIC] possible return addresses (stack scan, addr2line these):\n");
  panic_diag_drain();
  const uintptr_t code_start = XIP_BASE;
  const uintptr_t code_end = (uintptr_t)&__flash_binary_end;
  const uint32_t *scan = (const uint32_t *)((uintptr_t)entry_sp & ~(uintptr_t)3);
  constexpr uint32_t SCAN_WORDS = 256; // 1KiB ぶん (スレッドスタックは 4KiB)。
  constexpr uint32_t SHOW_MAX = 24;    // 出しすぎない。
  uint32_t shown = 0;
  for (uint32_t index = 0; index < SCAN_WORDS && shown < SHOW_MAX; ++index) {
    const uint32_t word = scan[index];
    if ((word & 1u) == 1u && (word - 1u) >= code_start &&
        (word - 1u) < code_end) {
      rp2350_pico2::diag_printf("  sp+%-4lu %08lx\n",
                                (unsigned long)(index * 4),
                                (unsigned long)word);
      panic_diag_drain();
      ++shown;
    }
  }
  rp2350_pico2::diag_printf("[PANIC] stopping the other core\n");
  panic_diag_drain();
  // ★★不変条件が壊れたカーネルの上で、もう一方のコアやこのコアの他スレッドが
  //   何ごとも無かったように動き続けるのは panic の意味そのものを壊す —
  //   「不変条件が壊れた」は「共有状態が信用できない」なので、系全体を止める
  //   のが筋 (I-9 / D12)。診断を出し切った**後で**試みる: 相手も一緒に壊れて
  //   応答が無ければここで戻ってこないが、それは「本当に相手も道連れで
  //   壊れている」という事実そのものなので、黙って動き続けるより正直。
  rp2350_pico2::park_other_cores();

  rp2350_pico2::diag_printf(
      "[PANIC] halting (only USB-related IRQs stay enabled)\n");
  panic_diag_drain();
  // ★★USB を保つのに要らない割り込み (GPIO・DMA・ペリフェラルすべて) は
  //   ここで止める。SysTick を止めるだけではスレッド**切り替え**は止まっても、
  //   これらのハンドラ自身は動き続けてしまう — 壊れたカーネルの上で誰が
  //   何を触るか分からない状態を残すのは panic の意味を壊す (ユーザー指摘)。
  //   park_other_cores() (SIO の割り込みを使う) より**後**に呼ぶこと —
  //   先に呼ぶと隣のコアを止める手段ごと消える。
  objects::usb_cdc_isolate_for_panic();
  // ★このカーネルで「次に誰かへ切り替える」きっかけは SysTick 発の PendSV
  //   だけ (D26 / ARCH::pend_context_switch)。自分から SVC も撃たない
  //   (YIELD すらしない) ので、SysTick さえ止めればこのコアは二度と他
  //   スレッドへ切り替わらない。
  //   ★★★最初は save_and_disable_interrupts() で割り込みを丸ごと止めて
  //   いたが、それだと USB の割り込みハンドラ自体 (isr_usbctrl) も止まり、
  //   tud_task() を手で呼んでも**そもそもイベントが積まれない**ので
  //   picotool の焼き直し要求に応えられなかった (実際に踏んだ: BOOTSEL
  //   ボタンでの物理復旧が要った) — ポーリングでは救えず、ハンドラ自体を
  //   生かす必要があった。
  ARCH_TYPE::timer_cancel();
  while (true) {
  }
}

// ★もう一方のコアを起こす。起こされた側の入口は「自分で BOARD::init(core) を
//   呼んでからスレッドモードへ移る」責任を持つ — 優先度・MPU・SysTick は
//   per-core banked なので、起こす側から設定してやることができない。
//   ★pico-sdk の multicore_launch_core1 は core1 に自前のスタックを与えて C 関数を
//     呼ばせる。そのスタックは**起動の足場**にすぎず、スレッドとしてのスタックは
//     オブジェクトランドから借りたものへ移る (D18)。
void rp2350_pico2::diag_mute(bool quiet) { g_diag_quiet = quiet; }

void rp2350_pico2::launch_core(void (*entry)()) {
  ::multicore_launch_core1(entry);
}

// ★止め方は「イベントを送って相手をブロックさせる」。pico-sdk の lockout は
//   SIO の FIFO 割り込みで相手を**今やっていることから引き剥がし**、RAM 上に
//   置かれたハンドラの中で待たせる。割り込みで引き剥がすところが要点で、
//   「相手が自分から確認しに来るのを待つ」形にすると、相手が flash 上のコードを
//   回している間は永久に来ない。
//   ★被害者側の仕込み (multicore_lockout_victim_init) は init() で**両コア**が行う
//     (片方だけだと、登録されていない側を相手にする flash 書き込みが assert する)。
// ★「2 コア構成か」をビルド旗で判定しない。**実際に起きているかを実行時に見る** —
//   起こす前に呼ばれても、1 コア構成でも、同じコードで正しく振る舞う
//   (旗で分けると「旗は立っているがまだ起きていない」瞬間に固まる)。
static bool g_parked = false;
// ★入れ子にできるようにする。「外側で 1 回だけ呼ぶよう気をつける」形にすると、
//   いつか必ず 1 本落とす (落とすと相手が止まったまま戻らない)。数えておけば、
//   1 ページごとに呼ぼうが 1 操作でまとめて囲もうが、実際に止まるのは 1 回で済む。
static uint32_t g_park_depth = 0;
// ★止める側は同時に 1 コアだけ。**2 コアが互いを止めようとすると即デッドロック**
//   する (どちらも相手の応答を待つ)。錠を取ってから止めるので、負けた側は
//   相手に止められて待ち、解放後に自分の番になる — 待ちは相手の操作 1 つぶんで有界。
static volatile uint32_t g_park_lock = 0;

void rp2350_pico2::park_other_cores() {
  if (g_park_depth++ != 0)
    return; // 既に止めてある (入れ子)
  const uint32_t other = ::get_core_num() == 0 ? 1u : 0u;
  if (!::multicore_lockout_victim_is_initialized(other))
    return; // 相手はまだ居ない = 止めるものが無い
  while (!ARCH_TYPE::cas32(&g_park_lock, 0u, 1u)) {
  }
  ::multicore_lockout_start_blocking();
  g_parked = true;
}

void rp2350_pico2::resume_other_cores() {
  if (g_park_depth == 0)
    return; // 対になっていない呼び出し (何もしない)
  if (--g_park_depth != 0)
    return; // まだ外側が握っている
  if (!g_parked)
    return;
  g_parked = false;
  ::multicore_lockout_end_blocking();
  ARCH_TYPE::store_release32(&g_park_lock, 0u);
}

// ★転送幅は「揃っていれば 32bit、でなければ 8bit」。揃っていない場所へ 32bit で
//   投げると転送そのものがずれるので、幅を上げるのは条件が揃ったときだけ。
void rp2350_pico2::dma_copy(int channel, const void *from, void *to,
                            uint32_t bytes) {
  const bool wide = ((uintptr_t)from % 4 == 0) && ((uintptr_t)to % 4 == 0) &&
                    (bytes % 4 == 0);
  dma_channel_config config = ::dma_channel_get_default_config(channel);
  ::channel_config_set_transfer_data_size(
      &config, wide ? DMA_SIZE_32 : DMA_SIZE_8);
  ::channel_config_set_read_increment(&config, true);
  ::channel_config_set_write_increment(&config, true);
  ::dma_channel_configure(channel, &config, to, from,
                          wide ? bytes / 4 : bytes, true);
}

int rp2350_pico2::dma_claim() { return ::dma_claim_unused_channel(false); }

bool rp2350_pico2::dma_busy(int channel) {
  return ::dma_channel_is_busy(channel);
}

} // namespace boards
} // namespace shizuku
