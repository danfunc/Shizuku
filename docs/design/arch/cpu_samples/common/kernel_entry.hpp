// サンプル: CPU 層からカーネルへ入る口は 4 本だけ。
//
// どの ISA でも、例外入口の asm はこの 4 本のどれかを 1 回 bl/call するだけにする。
// 今のコードは 1 回の svc で current_context / pending_step / restore_region_window /
// current_context と C を 4 回呼んでいる。窓・特権・スタック上限の設定は、
// 「次に走る文脈が決まった時点」でカーネルの C 側が自分で済ませる。
//
// 規約 (ISA 共通):
//   - SVC/ECALL は文脈を切り替えない。結果を frame に書く (in-place call なら
//     frame の pc/sp/特権と窓も書き換える) だけで、同じスレッドへ戻る。
//   - 切替えが要るときは pend だけ立てる。切替えは最低優先度の 1 か所
//     (ARM は PendSV、RISC-V は trap 出口の pend 確認) でしか起きない。
//   - だから SVC 入口は callee-saved (r4-r11 / s0-s11) を退避しない。C が守る。
//     呼び手の callee-saved を消す責任は caller/callee の converter にある
//     (D1 (c)、2026-10-27 決定)。
#ifndef SHIZUKU_SAMPLE_KERNEL_ENTRY_HPP
#define SHIZUKU_SAMPLE_KERNEL_ENTRY_HPP
#include <stdint.h>

extern "C" {

// SVC / ECALL。frame は ISA の例外フレーム (ARM はハードウェアが積んだもの、
// RISC-V は入口 asm が作った文脈ブロック)。戻り値は無い: 結果は frame に書く。
void shizuku_svc(void *frame);

// 切替え。current の退避が済んだ sp と、そのときの復帰情報 (ARM は EXC_RETURN) を
// 受け取り、次に走る文脈の sp と復帰情報を返す。
// ★復帰情報はスレッドのスタックに置かない。非特権スレッドが書き換えられる場所に
//   EXC_RETURN を置くと、ハンドラモードへの復帰を偽造できる。
// ARM: 戻り値 uint64_t の下位 32bit = sp (r0)、上位 = EXC_RETURN (r1)。
//      (AAPCS では 8 バイトの構造体はメモリ返しになるので、uint64_t で返す)
uint64_t shizuku_switch(uintptr_t saved_sp, uint32_t exc_return);

// 時限タイマの期限。切替えが要るなら pend を立てるだけ。
void shizuku_tick();

// フォールト。落ちたスレッドを止めて pend を立てる。戻り先は PendSV が差し替える。
void shizuku_fault(void *frame, uint32_t exc_return);

} // extern "C"
#endif
