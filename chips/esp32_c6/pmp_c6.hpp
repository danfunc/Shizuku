#ifndef SHIZUKU_CHIPS_ESP32_C6_PMP_C6_HPP
#define SHIZUKU_CHIPS_ESP32_C6_PMP_C6_HPP

#include <cstdint>
#include <cstddef>
#include "arch/riscv/pmp.hpp"

namespace shizuku::chips::esp32_c6 {

// 段1: ESP32-C6 PMP メモリ保護構成
// 物理エントリ 16 本、粒度 4 バイト (G=4)。
// allow-list 方式: 実行対象 object code (R|X)、object data (R|W, NX)、U stack (R|W, NX)、貸与 region (R|W, optional) のみ許可。
// 全メモリ許可は禁止。未マッチ領域は RISC-V PMP 仕様により U-mode からアクセス禁止 (Fault)。
// カーネルスタック / trusted stack / 復帰情報は U-mode から完全に隔離される。

struct c6_stage1_object_bounds {
  uint32_t owner_id = 0;
  uintptr_t code_base = 0;
  uintptr_t code_limit = 0;
  uintptr_t data_base = 0;
  uintptr_t data_limit = 0;
  uintptr_t stack_base = 0;
  uintptr_t stack_limit = 0;
  uintptr_t u_return_trampoline = 0;
};

// オブジェクト所有者の検証述語:
// 信頼された M-mode が設定した登録済み owner_id (!= 0) と、
// 要求された owner_id および現在実行中オブジェクト (current_obj) が厳密一致することを検証。
// 未登録 (0) または異なるオブジェクト ID は fail-closed で即座に拒否。
inline constexpr bool is_authorized_u_object(uint32_t registered_owner,
                                             uint32_t requested_owner,
                                             uint32_t current_obj) noexcept {
  return (registered_owner != 0 &&
          registered_owner == requested_owner &&
          current_obj == registered_owner);
}

// 信頼されたカーネルスタックの定数
constexpr size_t KSTACK_BYTES = 4096;
constexpr size_t KSTACK_GUARD_BYTES = 256;

// 信頼された C dispatch スタックの定数
constexpr size_t DISPATCH_STACK_BYTES = 4096;

// アドレスが trusted kernel stack 範囲内にあるか判定
inline constexpr bool is_kstack_range(uintptr_t addr, uintptr_t kstack_start, size_t kstack_bytes = KSTACK_BYTES) noexcept {
  if (kstack_start == 0 || kstack_bytes == 0) return false;
  return (addr >= kstack_start && addr <= kstack_start + kstack_bytes);
}

// アドレスが trusted dispatch stack 範囲内にあるか判定
inline constexpr bool is_dispatch_stack_range(uintptr_t addr, uintptr_t dstack_start, size_t dstack_bytes = DISPATCH_STACK_BYTES) noexcept {
  if (dstack_start == 0 || dstack_bytes == 0) return false;
  return (addr >= dstack_start && addr <= dstack_start + dstack_bytes);
}

// trusted kernel stack の下限 (ガード領域を含めた限界値)
inline constexpr uintptr_t compute_kstack_limit(uintptr_t kstack_start, size_t guard_bytes = KSTACK_GUARD_BYTES) noexcept {
  if (kstack_start == 0) return 0;
  return kstack_start + guard_bytes;
}

// U->M native API handler スタック切り替え述語:
// SP が U-mode スタック範囲内 [u_stack_base, u_stack_limit] にある場合、
// 特権 C スタックや保存 frame が U-readable スタックへ露出するのを防ぐため、
// ハンドラの実行時初期 SP (snapshot = frame_sp + RV32_FRAME_BYTES) を trusted kernel stack 上に設定する。
// trusted kernel stack の valid range [kstack_start + guard, kstack_top] および 16B 整列を厳格検証する。
inline constexpr uintptr_t get_m_handler_stack(uintptr_t current_sp,
                                               uintptr_t u_stack_base,
                                               uintptr_t u_stack_limit,
                                               uintptr_t frame_sp,
                                               uintptr_t kstack_start,
                                               size_t kstack_bytes = KSTACK_BYTES,
                                               size_t kstack_guard_bytes = KSTACK_GUARD_BYTES) noexcept {
  if (u_stack_base != 0 && current_sp >= u_stack_base && current_sp <= u_stack_limit) {
    if (kstack_start == 0 || kstack_bytes == 0) return 0;
    const uintptr_t kstack_top = kstack_start + kstack_bytes;
    const uintptr_t kstack_floor = kstack_start + kstack_guard_bytes;

    // source/kernel/dispatch.cpp:79-83, 129-135 実装契約:
    // callee C stack 初期 SP は snapshot = callee_frame + frame_bytes (ARCH::psp_after_return と厳密に一致)。
    // frame_sp 自体を返すと call_frame_header との間にズレが生じるため、snapshot を返す。
    constexpr uintptr_t FRAME_BYTES = 144u; // RV32_FRAME_BYTES
    const uintptr_t callee_stack_sp = frame_sp + FRAME_BYTES;

    // 16 バイト整列検証
    if ((callee_stack_sp & 15u) != 0) {
      return 0; // Fail-closed
    }
    // trusted kernel stack 境界検証: snapshot は [kstack_floor, kstack_top] に収まること
    if (callee_stack_sp <= kstack_floor || callee_stack_sp > kstack_top) {
      return 0; // Fail-closed
    }
    // callee_frame 自体も kstack_floor 以上にあること (headroom 保証)
    if (frame_sp < kstack_floor) {
      return 0; // Fail-closed (headroom 不足)
    }
    return callee_stack_sp;
  }
  return current_sp;
}

struct pmp_c6_config {
  uintptr_t code_base = 0;
  uintptr_t code_limit = 0;
  uintptr_t data_base = 0;
  uintptr_t data_limit = 0;
  uintptr_t stack_base = 0;
  uintptr_t stack_limit = 0;
  uintptr_t region_base = 0;
  uintptr_t region_limit = 0;
};

class pmp_controller {
public:
  using codec = shizuku::ports::riscv::pmp_codec<4>;

  // 2つの半開区間 [b1, l1) と [b2, l2) の重なり検査 (重なりがあれば true)
  static constexpr bool ranges_overlap(uintptr_t b1, uintptr_t l1, uintptr_t b2, uintptr_t l2) noexcept {
    if (l1 <= b1 || l2 <= b2) return false;
    return !(l1 <= b2 || b1 >= l2);
  }

  // 設定の安全性・不変条件検証 (ホスト単体テスト・ランタイム双方で実行可能)
  static constexpr bool validate(const pmp_c6_config &cfg,
                                 uintptr_t kernel_stack_start,
                                 uintptr_t kernel_stack_bytes,
                                 uintptr_t dispatch_stack_start = 0,
                                 uintptr_t dispatch_stack_bytes = 0) noexcept {
    // 0. カーネルスタックの算術オーバーフロー検査
    if (kernel_stack_bytes > 0) {
      const uint64_t full_sum = static_cast<uint64_t>(kernel_stack_start) + static_cast<uint64_t>(kernel_stack_bytes);
      if (full_sum > 0xFFFFFFFFull || full_sum < static_cast<uint64_t>(kernel_stack_start)) {
        return false; // overflow
      }
    }
    const uintptr_t kstack_end = kernel_stack_start + kernel_stack_bytes;

    // 0b. ディスパッチスタックの算術オーバーフロー検査
    if (dispatch_stack_bytes > 0) {
      const uint64_t full_dsum = static_cast<uint64_t>(dispatch_stack_start) + static_cast<uint64_t>(dispatch_stack_bytes);
      if (full_dsum > 0xFFFFFFFFull || full_dsum < static_cast<uint64_t>(dispatch_stack_start)) {
        return false; // overflow
      }
    }
    const uintptr_t dstack_end = dispatch_stack_start + dispatch_stack_bytes;

    // 1. コード領域の検証: 4バイト整列、base < limit、空でないこと
    if (cfg.code_base >= cfg.code_limit || (cfg.code_base & 3u) != 0 || (cfg.code_limit & 3u) != 0) {
      return false; // fail-closed
    }
    // 全メモリ許可 (0..4GB) や過大な領域の禁止
    if (cfg.code_base == 0 && cfg.code_limit >= 0x80000000u) {
      return false;
    }

    // 2. スタック領域の検証: 4バイト整列、base < limit、非空
    if (cfg.stack_base >= cfg.stack_limit || (cfg.stack_base & 3u) != 0 || (cfg.stack_limit & 3u) != 0) {
      return false; // fail-closed
    }
    if (cfg.stack_base == 0 && cfg.stack_limit >= 0x80000000u) {
      return false;
    }

    // 3. データ領域の検証 (optional: limit == 0 なら空)
    if (cfg.data_limit != 0) {
      if (cfg.data_base >= cfg.data_limit || (cfg.data_base & 3u) != 0 || (cfg.data_limit & 3u) != 0) {
        return false;
      }
      if (cfg.data_base == 0 && cfg.data_limit >= 0x80000000u) {
        return false;
      }
    } else {
      // 空値の意味: data_limit == 0 のとき data_base も 0 でなければならない
      if (cfg.data_base != 0) {
        return false;
      }
    }

    // 4. 貸与 region の検証 (optional: limit == 0 なら空)
    if (cfg.region_limit != 0) {
      if (cfg.region_base >= cfg.region_limit || (cfg.region_base & 3u) != 0 || (cfg.region_limit & 3u) != 0) {
        return false;
      }
      if (cfg.region_base == 0 && cfg.region_limit >= 0x80000000u) {
        return false;
      }
    } else {
      // 空値の意味: region_limit == 0 のとき region_base も 0 でなければならない
      if (cfg.region_base != 0) {
        return false;
      }
    }

    // 5. 4許可領域相互の全組み合わせの重複検査 (全6ペア)
    // 1) Code vs Stack
    if (ranges_overlap(cfg.code_base, cfg.code_limit, cfg.stack_base, cfg.stack_limit)) {
      return false;
    }
    // 2) Code vs Data
    if (cfg.data_limit != 0 && ranges_overlap(cfg.code_base, cfg.code_limit, cfg.data_base, cfg.data_limit)) {
      return false;
    }
    // 3) Code vs Region
    if (cfg.region_limit != 0 && ranges_overlap(cfg.code_base, cfg.code_limit, cfg.region_base, cfg.region_limit)) {
      return false;
    }
    // 4) Stack vs Data
    if (cfg.data_limit != 0 && ranges_overlap(cfg.stack_base, cfg.stack_limit, cfg.data_base, cfg.data_limit)) {
      return false;
    }
    // 5) Stack vs Region
    if (cfg.region_limit != 0 && ranges_overlap(cfg.stack_base, cfg.stack_limit, cfg.region_base, cfg.region_limit)) {
      return false;
    }
    // 6) Data vs Region
    if (cfg.data_limit != 0 && cfg.region_limit != 0 &&
        ranges_overlap(cfg.data_base, cfg.data_limit, cfg.region_base, cfg.region_limit)) {
      return false;
    }

    // 6. カーネル/信頼スタック範囲との全重複検査
    if (kernel_stack_bytes > 0) {
      if (ranges_overlap(cfg.code_base, cfg.code_limit, kernel_stack_start, kstack_end)) {
        return false;
      }
      if (ranges_overlap(cfg.stack_base, cfg.stack_limit, kernel_stack_start, kstack_end)) {
        return false;
      }
      if (cfg.data_limit != 0 && ranges_overlap(cfg.data_base, cfg.data_limit, kernel_stack_start, kstack_end)) {
        return false;
      }
      if (cfg.region_limit != 0 && ranges_overlap(cfg.region_base, cfg.region_limit, kernel_stack_start, kstack_end)) {
        return false;
      }
    }

    // 7. C ディスパッチスタック範囲との全重複検査
    if (dispatch_stack_bytes > 0) {
      if (ranges_overlap(cfg.code_base, cfg.code_limit, dispatch_stack_start, dstack_end)) {
        return false;
      }
      if (ranges_overlap(cfg.stack_base, cfg.stack_limit, dispatch_stack_start, dstack_end)) {
        return false;
      }
      if (cfg.data_limit != 0 && ranges_overlap(cfg.data_base, cfg.data_limit, dispatch_stack_start, dstack_end)) {
        return false;
      }
      if (cfg.region_limit != 0 && ranges_overlap(cfg.region_base, cfg.region_limit, dispatch_stack_start, dstack_end)) {
        return false;
      }
      // カーネルスタックとディスパッチスタック自体の完全非重複性
      if (kernel_stack_bytes > 0 && ranges_overlap(kernel_stack_start, kstack_end, dispatch_stack_start, dstack_end)) {
        return false;
      }
    }

    return true;
  }

  // ハードウェア CSR への書き込みと allow-list 適用
  static bool apply(const pmp_c6_config &cfg,
                    uintptr_t kernel_stack_start,
                    uintptr_t kernel_stack_bytes,
                    uintptr_t dispatch_stack_start = 0,
                    uintptr_t dispatch_stack_bytes = 0) noexcept {
    if (!validate(cfg, kernel_stack_start, kernel_stack_bytes, dispatch_stack_start, dispatch_stack_bytes)) {
      return false; // 不正設定時は適用拒否 (fail-closed)
    }

    namespace perm = shizuku::ports::riscv::pmp_perm;

    // Entry 0, 1: Code (TOR, R | X)
    shizuku::ports::riscv::pmp_entry e1{};
    if (!codec::encode_tor(cfg.code_limit, perm::R | perm::X, e1)) {
      return false;
    }

    // Entry 2, 3: Data (TOR, R | W, No Execute) (optional)
    uint8_t cfg2 = 0, cfg3 = 0;
    uint32_t addr2 = 0, addr3 = 0;
    if (cfg.data_limit != 0 && cfg.data_base < cfg.data_limit) {
      shizuku::ports::riscv::pmp_entry e3{};
      if (!codec::encode_tor(cfg.data_limit, perm::R | perm::W, e3)) {
        return false;
      }
      addr2 = cfg.data_base >> 2;
      addr3 = e3.addr;
      cfg2 = 0;
      cfg3 = e3.cfg;
    }

    // Entry 4, 5: U Stack (TOR, R | W, No Execute)
    shizuku::ports::riscv::pmp_entry e5{};
    if (!codec::encode_tor(cfg.stack_limit, perm::R | perm::W, e5)) {
      return false;
    }

    // Entry 6, 7: Region window (optional)
    uint8_t cfg6 = 0, cfg7 = 0;
    uint32_t addr6 = 0, addr7 = 0;
    if (cfg.region_limit != 0 && cfg.region_base < cfg.region_limit) {
      shizuku::ports::riscv::pmp_entry e7{};
      if (!codec::encode_tor(cfg.region_limit, perm::R | perm::W, e7)) {
        return false;
      }
      addr6 = cfg.region_base >> 2;
      addr7 = e7.addr;
      cfg6 = 0;
      cfg7 = e7.cfg;
    }

    const uint32_t pmpcfg0_val = codec::pack_pmpcfg(0, e1.cfg, cfg2, cfg3);
    const uint32_t pmpcfg1_val = codec::pack_pmpcfg(0, e5.cfg, cfg6, cfg7);

#if defined(__riscv)
    asm volatile(
        "csrw 0x3B0, %0\n" // pmpaddr0 (code base)
        "csrw 0x3B1, %1\n" // pmpaddr1 (code limit)
        "csrw 0x3B2, %2\n" // pmpaddr2 (data base)
        "csrw 0x3B3, %3\n" // pmpaddr3 (data limit)
        "csrw 0x3B4, %4\n" // pmpaddr4 (stack base)
        "csrw 0x3B5, %5\n" // pmpaddr5 (stack limit)
        "csrw 0x3B6, %6\n" // pmpaddr6 (region base)
        "csrw 0x3B7, %7\n" // pmpaddr7 (region limit)
        "csrw 0x3A0, %8\n" // pmpcfg0 (code & data)
        "csrw 0x3A1, %9\n" // pmpcfg1 (stack & region)
        "csrw 0x3A2, zero\n" // pmpcfg2 = OFF
        "csrw 0x3A3, zero\n" // pmpcfg3 = OFF
        "fence.i\n"
        "fence rw,rw\n"
        :
        : "r"(cfg.code_base >> 2), "r"(e1.addr),
          "r"(addr2), "r"(addr3),
          "r"(cfg.stack_base >> 2), "r"(e5.addr),
          "r"(addr6), "r"(addr7),
          "r"(pmpcfg0_val), "r"(pmpcfg1_val)
        : "memory");
#else
    (void)pmpcfg0_val;
    (void)pmpcfg1_val;
    (void)addr2;
    (void)addr3;
    (void)addr6;
    (void)addr7;
#endif

    return true;
  }

  // PMP を全 OFF (M-mode 初期状態)
  static void disable() noexcept {
#if defined(__riscv)
    asm volatile(
        "csrw 0x3A0, zero\n"
        "csrw 0x3A1, zero\n"
        "csrw 0x3A2, zero\n"
        "csrw 0x3A3, zero\n"
        "fence.i\n"
        "fence rw,rw\n"
        ::: "memory");
#endif
  }
};

} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_PMP_C6_HPP
