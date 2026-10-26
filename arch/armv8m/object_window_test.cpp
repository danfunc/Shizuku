// RP2350 (ARMv8-M) の非特権オブジェクト保護窓: MPU レジスタには触らず、前計算した
// RBAR/RLAR の値と、戻り先の権威 (特権台帳) の記録・復元だけを host で縛る。
#include "armv8m.hpp"

#include <cassert>

#include "shizuku/templates/thread.hpp"

using arch = shizuku::archs::armv8m<8, 4>;
using thread_t = shizuku::templates::thread<arch::context_t>;

int main() {
  // ---- データ窓: 32B 単位に上丸め、RW_ALL・XN・Attr1 (通常メモリ) ----
  {
    const auto w = arch::encode_data_window(0x20010020u, 0x20010060u);
    assert(w.rbar == (0x20010020u | (arch::ACCESS_RW_ALL << 1) | 1u));
    // RLAR.LIMIT は含む側の最後の 32B ブロック (0x20010040..0x2001005F) を指す。排他的な終端
    // 0x20010060 をそのまま入れると 1 ブロック余分に開いて隣のオブジェクトに届く。
    assert(w.rlar == (0x20010040u | (1u << 1) | 1u));
    // 末尾が 32B 境界でないときは、そのバイトを含むブロックまで上丸め (要求範囲を丸ごと覆う)。
    const auto up = arch::encode_data_window(0x20010020u, 0x20010041u);
    assert((up.rlar & ~0x1Fu) == 0x20010040u);
    // 隣接する 256B ずつの 2 つの窓が重ならない (A の最後のブロックは B の先頭の手前)。
    const auto a = arch::encode_data_window(0x20010000u, 0x20010100u);
    const auto b = arch::encode_data_window(0x20010100u, 0x20010200u);
    assert((a.rlar & ~0x1Fu) == 0x200100E0u && (b.rbar & ~0x1Fu) == 0x20010100u);
    // 空・逆転・base==0 は「窓なし」(rlar==0 = 無効) で、広い窓に化けない。
    assert(arch::encode_data_window(0x20010000u, 0x20010000u).rlar == 0);
    assert(arch::encode_data_window(0x20010100u, 0x20010000u).rlar == 0);
    assert(arch::encode_data_window(0, 0x100u).rlar == 0);
  }

  // ---- MMIO 窓: Device 属性 (Attr2)。DMA / PIO / PPB は窓を発行しない ----
  {
    const auto uart = arch::encode_mmio_window(0x40070000u, 0x40071000u);
    assert(uart.rlar != 0);
    assert(((uart.rlar >> 1) & 7u) == 2u);
    assert(arch::encode_mmio_window(0x50000000u, 0x50001000u).rlar == 0);  // DMA
    assert(arch::encode_mmio_window(0x50200000u, 0x50201000u).rlar == 0);  // PIO0
    assert(arch::encode_mmio_window(0xE000E000u, 0xE000F000u).rlar == 0);  // NVIC / SCB
    // 禁止範囲に 1 バイトでもかかる窓は全体を拒否する (一部だけ削って渡さない)。
    assert(arch::encode_mmio_window(0x4FFFF000u, 0x50000100u).rlar == 0);
  }

  // ---- stack 窓はスレッド側の値で、データ窓と同じ形 ----
  {
    const auto s = arch::encode_stack_window(0x20020000u, 0x20020800u);
    assert(s.rbar == (0x20020000u | (arch::ACCESS_RW_ALL << 1) | 1u));
    assert((s.rlar & ~0x1Fu) == 0x200207E0u);
  }

  // ---- 貸与窓: LIMIT は終端を含む最後の 32B ブロック (終端ちょうどの次のブロックは開けない) ----
  {
    assert(arch::grant_window_limit(0x20010100u) == 0x200100E0u);  // 32B 境界の終端
    assert(arch::grant_window_limit(0x20010101u) == 0x20010100u);  // 1 バイトはみ出す終端
    assert(arch::grant_window_limit(0x20010001u) == 0x20010000u);
  }

  // ---- 窓の表: 上限内だけ登録でき、上限外は false (黙って空窓にしない) ----
  {
    assert(arch::set_object_bounds(5, 0x20030000u, 0x20030100u, 0, 0));
    assert(arch::has_object_bounds(5));
    assert(!arch::has_object_bounds(6));
    assert(!arch::set_object_bounds(arch::kMaxObjectWindows, 0x20030000u, 0x20030100u, 0, 0));
    assert(!arch::has_object_bounds(arch::kMaxObjectWindows));
  }

  // ---- 戻り先の権威: 台帳に記録した値がそのまま復元され、範囲外は false ----
  {
    static shizuku::caller_return_record records[34];
    thread_t t;
    t.return_records = records;
    assert(thread_t::ledger_accessor::record_caller_return_authority(
        t, 3, 0x1001u, 0x20001000u, 1, 0x20000000u, 0x20000100u, 2, 0));
    uintptr_t pc = 0, sp = 0, base = 0, limit = 0;
    bool priv = true;
    assert(thread_t::ledger_accessor::restore_caller_return_authority(t, 3, pc, sp, priv, base,
                                                                      limit));
    // 記録した ret_privileged=1 (= 戻り先は特権) がそのまま特権として戻る。
    assert(pc == 0x1001u && sp == 0x20001000u && priv);
    assert(base == 0x20000000u && limit == 0x20000100u);
    // 非特権の戻り先 (ret_privileged=0) は非特権として戻る。
    assert(thread_t::ledger_accessor::record_caller_return_authority(t, 4, 0x2001u, 0x20002000u, 0,
                                                                    0, 0, 1, 0));
    assert(thread_t::ledger_accessor::restore_caller_return_authority(t, 4, pc, sp, priv, base,
                                                                      limit));
    assert(pc == 0x2001u && sp == 0x20002000u && !priv);
    // 範囲外の段は記録も復元も失敗する (user stack の値に逃げない)。
    assert(!thread_t::ledger_accessor::record_caller_return_authority(t, 34, 1, 1, 0, 0, 0, 0, 0));
    assert(!thread_t::ledger_accessor::restore_caller_return_authority(t, 34, pc, sp, priv, base,
                                                                       limit));
    // depth 0 の push 前に pop 側が depth-1 を引いても (unsigned 回り込み) 失敗する。
    assert(!thread_t::ledger_accessor::restore_caller_return_authority(
        t, static_cast<uint32_t>(0) - 1u, pc, sp, priv, base, limit));
    // 退避域が無いスレッドは記録できない。
    thread_t none;
    assert(!thread_t::ledger_accessor::record_caller_return_authority(none, 0, 1, 1, 0, 0, 0, 0,
                                                                      0));
  }
  return 0;
}
