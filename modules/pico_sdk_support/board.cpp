#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/exception.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "hardware/dma.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/boards/rp2350_pico2.hpp"

// board は「どの ISA の上に居るか」を知っている層でもある (region を張る機構は
// arch 側)。ここだけの別名にして、上位のテンプレート引数とは混ぜない。
using ARCH_TYPE = shizuku::archs::armv8m;

// リンカが置く「静的データの終端 = ヒープの先頭」。名前空間の中で宣言すると
// 名前が飾られて別物になるので、必ずファイル先頭で C リンケージとして宣言する。
extern "C" char __end__[];
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
  if (core != 0) {
    // ★このコアを「止められる側」として仕込む。FIFO 割り込みのハンドラは RAM に
    //   置かれており、止められている間 flash を 1 バイトも読まない。
    ::multicore_lockout_victim_init();
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
  // region0: XIP (フラッシュ窓) 全域 = 読み+実行のみ。フラッシュへの誤書き込みを止める。
  ARCH_TYPE::region_set(0, XIP_BASE, XIP_END - 32u, ARCH_TYPE::ACCESS_RO_ALL,
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
  va_list args;
  va_start(args, format);
  ::vprintf(format, args);
  va_end(args);
}

void rp2350_pico2::panic(const char *message) {
  ::panic("%s", message);
}

// ★もう一方のコアを起こす。起こされた側の入口は「自分で BOARD::init(core) を
//   呼んでからスレッドモードへ移る」責任を持つ — 優先度・MPU・SysTick は
//   per-core banked なので、起こす側から設定してやることができない。
//   ★pico-sdk の multicore_launch_core1 は core1 に自前のスタックを与えて C 関数を
//     呼ばせる。そのスタックは**起動の足場**にすぎず、スレッドとしてのスタックは
//     オブジェクトランドから借りたものへ移る (D18)。
void rp2350_pico2::launch_core(void (*entry)()) {
  ::multicore_launch_core1(entry);
}

// ★止め方は「イベントを送って相手をブロックさせる」。pico-sdk の lockout は
//   SIO の FIFO 割り込みで相手を**今やっていることから引き剥がし**、RAM 上に
//   置かれたハンドラの中で待たせる。割り込みで引き剥がすところが要点で、
//   「相手が自分から確認しに来るのを待つ」形にすると、相手が flash 上のコードを
//   回している間は永久に来ない。
//   ★被害者側の仕込み (multicore_lockout_victim_init) は init(core != 0) で行う。
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
