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

enum class c6_stage1_role : uint8_t {
  Root = 0,
  App = 1,
  Driver = 2,
};

struct alignas(8) pmp_precomputed_csr {
  uint32_t pmpaddr4 = 0; // stack base >> 2
  uint32_t pmpaddr5 = 0; // stack limit >> 2
  uint32_t pmpaddr6 = 0; // data base >> 2
  uint32_t pmpaddr7 = 0; // data limit >> 2
  uint32_t pmpaddr8 = 0; // mmio base >> 2
  uint32_t pmpaddr9 = 0; // mmio limit >> 2
  uint32_t pmpcfg1 = 0;  // entries 4..7 cfg
  uint32_t pmpcfg2 = 0;  // entries 8..11 cfg
};

inline constexpr pmp_precomputed_csr precompute_object_pmp(
    uintptr_t stack_base, uintptr_t stack_limit,
    uintptr_t data_base, uintptr_t data_limit,
    c6_stage1_role role, uintptr_t mmio_base, uintptr_t mmio_limit) noexcept {
  pmp_precomputed_csr csr{};
  namespace perm = shizuku::ports::riscv::pmp_perm;
  constexpr uint8_t PMP_MODE_TOR = static_cast<uint8_t>(shizuku::ports::riscv::pmp_mode::TOR) << 3;

  // Entry 4, 5: Stack (TOR, R|W, NX)
  csr.pmpaddr4 = static_cast<uint32_t>(stack_base >> 2);
  csr.pmpaddr5 = static_cast<uint32_t>(stack_limit >> 2);
  const uint8_t cfg_stack = PMP_MODE_TOR | perm::R | perm::W;

  // Entry 6, 7: Data (TOR, R|W, NX) (optional)
  uint8_t cfg_data = 0;
  if (data_limit != 0 && data_base < data_limit) {
    csr.pmpaddr6 = static_cast<uint32_t>(data_base >> 2);
    csr.pmpaddr7 = static_cast<uint32_t>(data_limit >> 2);
    cfg_data = PMP_MODE_TOR | perm::R | perm::W;
  }

  // pmpcfg1: entry 4 (0), entry 5 (cfg_stack), entry 6 (0), entry 7 (cfg_data)
  csr.pmpcfg1 = shizuku::ports::riscv::pmp_codec<4>::pack_pmpcfg(0, cfg_stack, 0, cfg_data);

  // Entry 8, 9: MMIO (TOR, R|W, NX) (Driver のみ)
  uint8_t cfg_mmio = 0;
  if (role == c6_stage1_role::Driver && mmio_limit != 0 && mmio_base < mmio_limit) {
    csr.pmpaddr8 = static_cast<uint32_t>(mmio_base >> 2);
    csr.pmpaddr9 = static_cast<uint32_t>(mmio_limit >> 2);
    cfg_mmio = PMP_MODE_TOR | perm::R | perm::W;
  }

  // pmpcfg2: entry 8 (0), entry 9 (cfg_mmio), entry 10 (0), entry 11 (0)
  csr.pmpcfg2 = shizuku::ports::riscv::pmp_codec<4>::pack_pmpcfg(0, cfg_mmio, 0, 0);

  return csr;
}

struct c6_stage1_object_bounds {
  uint32_t owner_id = 0;
  uintptr_t code_base = 0;
  uintptr_t code_limit = 0;
  uintptr_t data_base = 0;
  uintptr_t data_limit = 0;
  uintptr_t stack_base = 0;
  uintptr_t stack_limit = 0;
  uintptr_t u_return_trampoline = 0;
  c6_stage1_role role = c6_stage1_role::App;
  uintptr_t mmio_base = 0;
  uintptr_t mmio_limit = 0;
  pmp_precomputed_csr precomputed{};
  // precomputed を作り直すたびに進める版。trap 側の「今入っている固有 PMP」cache は
  // (bounds, pmp_epoch) の組で照合するので、同じ bounds の中身を書き換えても古い窓が残らない。
  uint32_t pmp_epoch = 0;

  constexpr void update_precomputed() noexcept {
    precomputed = precompute_object_pmp(
        stack_base, stack_limit, data_base, data_limit, role, mmio_base, mmio_limit);
    pmp_epoch = pmp_epoch + 1u;
  }
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
  uintptr_t mmio_base = 0;
  uintptr_t mmio_limit = 0;
};

// ---- 段1 PMA/PMP 所有権の検証 (fail-closed、experiment 専用) ----
// PMA bit map: TRM v1.2 §1.9.5 p.69 (rendered PDF で確認済み): A=bits31:30 (TOR=01), LOCK=bit29,
// READ=bit4, WRITE=bit3, EXECUTE=bit2, TYPE=bit0 (1=valid)。PMA の R/W/X は特権モードに依らず適用 (TRM §1.9.3 p.67)。
// entry 0-12 は ESP-IDF v6.1 esp_cpu_configure_invalid_regions の定数 (r6 ELF 逆アセンブル) を bit-exact に保存する。
namespace pma_c6 {
constexpr uint32_t CFG_A_TOR = 0x40000000u;
constexpr uint32_t CFG_LOCK = 0x20000000u;
constexpr uint32_t CFG_R = 0x10u;
constexpr uint32_t CFG_W = 0x08u;
constexpr uint32_t CFG_X = 0x04u;
constexpr uint32_t CFG_VALID = 0x01u;
constexpr uint32_t IDF_INVALID_COUNT = 13;
// IDF v6.1 の定数 (r6 ELF の esp_cpu_configure_invalid_regions。addr は pma_addr の値 = アドレス >> 2)
constexpr uint32_t IDF_INVALID_ADDR[IDF_INVALID_COUNT] = {
    0x08000000u, 0x0C000000u, 0x10000000u, 0x10014000u, 0x10200000u, 0x10220000u, 0x10800000u,
    0x10A00000u, 0x14000000u, 0x14001000u, 0x18000000u, 0x18040000u, 0x3FFFFFFFu};
constexpr uint32_t IDF_INVALID_CFG[IDF_INVALID_COUNT] = {
    0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u,
    0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u};
// C6 HP_MEM (TRM v1.2 §5.3 memory map, 0x4080_0000-0x4087_FFFF)
constexpr uintptr_t HP_MEM_LOW = 0x40800000u;
constexpr uintptr_t HP_MEM_HIGH = 0x40880000u;

struct entry {
  uint32_t cfg;
  uint32_t addr;
};

constexpr uint32_t to_pmaddr(uintptr_t a) noexcept { return static_cast<uint32_t>(a >> 2); }
// entry 13: OFF のまま LOCK で pma_addr13 (entry14 の TOR 下限) を固定する
constexpr entry expected13(uintptr_t iram_low) noexcept { return {CFG_LOCK, to_pmaddr(iram_low)}; }
// entry 14: IRAM text [iram_low, iram_text_end) = R|X, W 無し (M 書込み禁止)
constexpr entry expected14(uintptr_t iram_text_end) noexcept {
  return {CFG_A_TOR | CFG_LOCK | CFG_R | CFG_X | CFG_VALID, to_pmaddr(iram_text_end)};
}
// entry 15: DRAM [iram_text_end, dram_high) = R|W, X 無し (M 実行禁止、U スタックも NX)
constexpr entry expected15(uintptr_t dram_high) noexcept {
  return {CFG_A_TOR | CFG_LOCK | CFG_R | CFG_W | CFG_VALID, to_pmaddr(dram_high)};
}

// 半開区間 [b1, l1) と [b2, l2) の重なり
constexpr bool ranges_overlap(uintptr_t b1, uintptr_t l1, uintptr_t b2, uintptr_t l2) noexcept {
  return b1 < l2 && b2 < l1;
}

// 新規 2 規則 (entry 14/15) の境界が整列・順序通りで、HP_MEM 内にあり、
// IDF の deny (TOR) 範囲 (entry 0-12 のうち A=TOR のもの) と重ならないことを確認する。
// 重なりが無ければ優先度 (最小番号一致) に依存しない。
constexpr bool layout_valid(uintptr_t iram_low, uintptr_t iram_text_end, uintptr_t dram_high) noexcept {
  if ((iram_low & 3u) != 0 || (iram_text_end & 3u) != 0 || (dram_high & 3u) != 0) return false;
  if (!(iram_low < iram_text_end && iram_text_end < dram_high)) return false;
  if (iram_low < HP_MEM_LOW || dram_high > HP_MEM_HIGH) return false;
  for (uint32_t i = 0; i < IDF_INVALID_COUNT; ++i) {
    if ((IDF_INVALID_CFG[i] & 0xC0000000u) != CFG_A_TOR) continue;  // OFF は範囲を持たない
    const uintptr_t lo = (i == 0) ? 0 : (static_cast<uintptr_t>(IDF_INVALID_ADDR[i - 1]) << 2);
    const uintptr_t hi = static_cast<uintptr_t>(IDF_INVALID_ADDR[i]) << 2;
    if (ranges_overlap(iram_low, dram_high, lo, hi)) return false;
  }
  return true;
}

// PMA の LOCK ビットを持つか (cfg の bit29)
constexpr bool cfg_locked(uint32_t cfg) noexcept { return (cfg & CFG_LOCK) != 0; }

// Stage 1 PMA ready marker: app の C wrapper (shizuku_c6_region.c) が PMA 0-15 を exact に読み直した後にだけ
// この値を書く。0 (未設定) や別の値は「PMA 規則が無い」とみなし、U-mode へ入らない (fail-closed)。
// C 側の定義は SHIZUKU_C6_STAGE1_PMA_READY_MAGIC (値は一致させる。test_trap_static.py が比較する)。
constexpr uint32_t READY_MAGIC = 0x5A1F0A11u;
constexpr bool ready_marker_ok(uint32_t marker) noexcept { return marker == READY_MAGIC; }
}  // namespace pma_c6

// PMP/PMA の CSR スナップショット (読取り専用)
struct pmp_snapshot {
  uint32_t cfg[4];
  uint32_t addr[16];
};
// pmpcfg のいずれかの byte に L (bit7) が立っているか
constexpr bool pmpcfg_has_lock(uint32_t word) noexcept { return (word & 0x80808080u) != 0; }
// pmpcfg0-3 の全 entry が unlock (L=0) か。1 つでも L=1 があれば所有権を取れない (pure、host で検証可能)。
constexpr bool pmp_cfg_all_unlocked(const uint32_t cfg[4]) noexcept {
  return !pmpcfg_has_lock(cfg[0]) && !pmpcfg_has_lock(cfg[1]) && !pmpcfg_has_lock(cfg[2]) && !pmpcfg_has_lock(cfg[3]);
}

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

    // 4b. MMIO window の検証 (optional: limit == 0 なら空)
    if (cfg.mmio_limit != 0) {
      if (cfg.mmio_base >= cfg.mmio_limit || (cfg.mmio_base & 3u) != 0 || (cfg.mmio_limit & 3u) != 0) {
        return false;
      }
    } else {
      if (cfg.mmio_base != 0) {
        return false;
      }
    }

    // 5. 5許可領域相互の重複検査
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
    // 7) MMIO vs 他領域 (Code, Stack, Data, Region)
    if (cfg.mmio_limit != 0) {
      if (ranges_overlap(cfg.code_base, cfg.code_limit, cfg.mmio_base, cfg.mmio_limit) ||
          ranges_overlap(cfg.stack_base, cfg.stack_limit, cfg.mmio_base, cfg.mmio_limit) ||
          (cfg.data_limit != 0 && ranges_overlap(cfg.data_base, cfg.data_limit, cfg.mmio_base, cfg.mmio_limit)) ||
          (cfg.region_limit != 0 && ranges_overlap(cfg.region_base, cfg.region_limit, cfg.mmio_base, cfg.mmio_limit))) {
        return false;
      }
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
      if (cfg.mmio_limit != 0 && ranges_overlap(cfg.mmio_base, cfg.mmio_limit, kernel_stack_start, kstack_end)) {
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
      if (cfg.mmio_limit != 0 && ranges_overlap(cfg.mmio_base, cfg.mmio_limit, dispatch_stack_start, dstack_end)) {
        return false;
      }
      // カーネルスタックとディスパッチスタック自体の完全非重複性
      if (kernel_stack_bytes > 0 && ranges_overlap(kernel_stack_start, kstack_end, dispatch_stack_start, dstack_end)) {
        return false;
      }
    }

    return true;
  }

  // 切替え時の定数縛り (試験で検証)
  static constexpr uint32_t SWITCH_CSR_COUNT = 8u;
  static constexpr uint32_t FIXED_ENTRIES_TOUCHED_ON_SWITCH = 0u;

  // 起動時の固定エントリ設定 (Entry 0-3: 共有コード・Trampoline, 共有アドレス窓)
  static void init_fixed_entries(uintptr_t code_base, uintptr_t code_limit,
                                 uintptr_t shared_region_base = 0,
                                 uintptr_t shared_region_limit = 0) noexcept {
    namespace perm = shizuku::ports::riscv::pmp_perm;
    constexpr uint8_t PMP_MODE_TOR = static_cast<uint8_t>(shizuku::ports::riscv::pmp_mode::TOR) << 3;
    const uint32_t addr0 = static_cast<uint32_t>(code_base >> 2);
    const uint32_t addr1 = static_cast<uint32_t>(code_limit >> 2);
    const uint8_t cfg_code = PMP_MODE_TOR | perm::R | perm::X;

    uint32_t addr2 = 0;
    uint32_t addr3 = 0;
    uint8_t cfg_shared = 0;
    if (shared_region_limit != 0 && shared_region_base < shared_region_limit) {
      addr2 = static_cast<uint32_t>(shared_region_base >> 2);
      addr3 = static_cast<uint32_t>(shared_region_limit >> 2);
      cfg_shared = PMP_MODE_TOR | perm::R | perm::W;
    }

    const uint32_t pmpcfg0_val = codec::pack_pmpcfg(0, cfg_code, 0, cfg_shared);

#if defined(__riscv)
    asm volatile(
        "csrw 0x3B0, %0\n" // pmpaddr0
        "csrw 0x3B1, %1\n" // pmpaddr1
        "csrw 0x3B2, %2\n" // pmpaddr2
        "csrw 0x3B3, %3\n" // pmpaddr3
        "csrw 0x3A0, %4\n" // pmpcfg0
        "csrw 0x3A3, zero\n" // pmpcfg3 (entries 12-15 = OFF)
        "fence.i\n"
        "fence rw,rw\n"
        :
        : "r"(addr0), "r"(addr1), "r"(addr2), "r"(addr3), "r"(pmpcfg0_val)
        : "memory");
#else
    (void)addr0; (void)addr1; (void)addr2; (void)addr3; (void)pmpcfg0_val;
#endif
  }

  // オブジェクト切替え (固定エントリに触れず、前計算された固有 8 本の CSR のみ書き込み)
  [[gnu::always_inline]] static void switch_to_precomputed(const pmp_precomputed_csr &csr) noexcept {
#if defined(__riscv)
    asm volatile(
        "csrw 0x3B4, %0\n" // pmpaddr4
        "csrw 0x3B5, %1\n" // pmpaddr5
        "csrw 0x3B6, %2\n" // pmpaddr6
        "csrw 0x3B7, %3\n" // pmpaddr7
        "csrw 0x3B8, %4\n" // pmpaddr8
        "csrw 0x3B9, %5\n" // pmpaddr9
        "csrw 0x3A1, %6\n" // pmpcfg1
        "csrw 0x3A2, %7\n" // pmpcfg2
        "fence.i\n"
        "fence rw,rw\n"
        :
        : "r"(csr.pmpaddr4), "r"(csr.pmpaddr5),
          "r"(csr.pmpaddr6), "r"(csr.pmpaddr7),
          "r"(csr.pmpaddr8), "r"(csr.pmpaddr9),
          "r"(csr.pmpcfg1), "r"(csr.pmpcfg2)
        : "memory");
#else
    (void)csr;
#endif
  }

  // PMP CSR の読み戻し (pmpcfg0-3, pmpaddr0-15)。host では 0 を返す。
  static pmp_snapshot read_pmp() noexcept {
    pmp_snapshot s{};
#if defined(__riscv)
    asm volatile("csrr %0, 0x3A0" : "=r"(s.cfg[0]));
    asm volatile("csrr %0, 0x3A1" : "=r"(s.cfg[1]));
    asm volatile("csrr %0, 0x3A2" : "=r"(s.cfg[2]));
    asm volatile("csrr %0, 0x3A3" : "=r"(s.cfg[3]));
    asm volatile("csrr %0, 0x3B0" : "=r"(s.addr[0]));
    asm volatile("csrr %0, 0x3B1" : "=r"(s.addr[1]));
    asm volatile("csrr %0, 0x3B2" : "=r"(s.addr[2]));
    asm volatile("csrr %0, 0x3B3" : "=r"(s.addr[3]));
    asm volatile("csrr %0, 0x3B4" : "=r"(s.addr[4]));
    asm volatile("csrr %0, 0x3B5" : "=r"(s.addr[5]));
    asm volatile("csrr %0, 0x3B6" : "=r"(s.addr[6]));
    asm volatile("csrr %0, 0x3B7" : "=r"(s.addr[7]));
    asm volatile("csrr %0, 0x3B8" : "=r"(s.addr[8]));
    asm volatile("csrr %0, 0x3B9" : "=r"(s.addr[9]));
    asm volatile("csrr %0, 0x3BA" : "=r"(s.addr[10]));
    asm volatile("csrr %0, 0x3BB" : "=r"(s.addr[11]));
    asm volatile("csrr %0, 0x3BC" : "=r"(s.addr[12]));
    asm volatile("csrr %0, 0x3BD" : "=r"(s.addr[13]));
    asm volatile("csrr %0, 0x3BE" : "=r"(s.addr[14]));
    asm volatile("csrr %0, 0x3BF" : "=r"(s.addr[15]));
#endif
    return s;
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

    // Entry 8, 9: MMIO window (optional, for Driver)
    uint8_t cfg8 = 0, cfg9 = 0;
    uint32_t addr8 = 0, addr9 = 0;
    if (cfg.mmio_limit != 0 && cfg.mmio_base < cfg.mmio_limit) {
      shizuku::ports::riscv::pmp_entry e9{};
      if (!codec::encode_tor(cfg.mmio_limit, perm::R | perm::W, e9)) {
        return false;
      }
      addr8 = cfg.mmio_base >> 2;
      addr9 = e9.addr;
      cfg8 = 0;
      cfg9 = e9.cfg;
    }

    const uint32_t pmpcfg0_val = codec::pack_pmpcfg(0, e1.cfg, cfg2, cfg3);
    const uint32_t pmpcfg1_val = codec::pack_pmpcfg(0, e5.cfg, cfg6, cfg7);
    const uint32_t pmpcfg2_val = codec::pack_pmpcfg(cfg8, cfg9, 0, 0);

#if defined(__riscv)
    // 事前: 全 entry が unlock (L=0) でなければ書込みは無視されうるため拒否 (fail-closed)
    const pmp_snapshot pre = read_pmp();
    if (!pmp_cfg_all_unlocked(pre.cfg)) return false;
    asm volatile(
        "csrw 0x3B0, %0\n" // pmpaddr0 (code base)
        "csrw 0x3B1, %1\n" // pmpaddr1 (code limit)
        "csrw 0x3B2, %2\n" // pmpaddr2 (data base)
        "csrw 0x3B3, %3\n" // pmpaddr3 (data limit)
        "csrw 0x3B4, %4\n" // pmpaddr4 (stack base)
        "csrw 0x3B5, %5\n" // pmpaddr5 (stack limit)
        "csrw 0x3B6, %6\n" // pmpaddr6 (region base)
        "csrw 0x3B7, %7\n" // pmpaddr7 (region limit)
        "csrw 0x3B8, %8\n" // pmpaddr8 (mmio base)
        "csrw 0x3B9, %9\n" // pmpaddr9 (mmio limit)
        "csrw 0x3A0, %10\n" // pmpcfg0 (code & data)
        "csrw 0x3A1, %11\n" // pmpcfg1 (stack & region)
        "csrw 0x3A2, %12\n" // pmpcfg2 (mmio)
        "csrw 0x3A3, zero\n" // pmpcfg3 = OFF
        "fence.i\n"
        "fence rw,rw\n"
        :
        : "r"(cfg.code_base >> 2), "r"(e1.addr),
          "r"(addr2), "r"(addr3),
          "r"(cfg.stack_base >> 2), "r"(e5.addr),
          "r"(addr6), "r"(addr7),
          "r"(addr8), "r"(addr9),
          "r"(pmpcfg0_val), "r"(pmpcfg1_val), "r"(pmpcfg2_val)
        : "memory");
    // 事後読み戻し: 無視された書込みや想定外の値は成功扱いにしない
    const pmp_snapshot post = read_pmp();
    if (post.cfg[0] != pmpcfg0_val || post.cfg[1] != pmpcfg1_val || post.cfg[2] != pmpcfg2_val || post.cfg[3] != 0) {
      return false;
    }
    const uint32_t exp_addr[10] = {static_cast<uint32_t>(cfg.code_base >> 2), e1.addr, addr2, addr3,
                                   static_cast<uint32_t>(cfg.stack_base >> 2), e5.addr, addr6, addr7,
                                   addr8, addr9};
    for (uint32_t i = 0; i < 10; ++i) {
      if (post.addr[i] != exp_addr[i]) return false;
    }
    for (uint32_t i = 10; i < 16; ++i) {  // entry 10-15 は書かないので事前値のまま
      if (post.addr[i] != pre.addr[i]) return false;
    }
#else
    (void)pmpcfg0_val;
    (void)pmpcfg1_val;
    (void)pmpcfg2_val;
    (void)addr2;
    (void)addr3;
    (void)addr6;
    (void)addr7;
    (void)addr8;
    (void)addr9;
    return false; // host: 実機の CSR を設定していない。成功扱いにしない (unsupported)
#endif

    return true;
  }

  // PMP を全 OFF (M-mode 初期状態)
  // 戻り値 false は CSR が 0 にならなかったこと (無視された書込み)。呼び出し側は U-mode へ戻らず停止する。
  static bool disable() noexcept {
#if defined(__riscv)
    asm volatile(
        "csrw 0x3A0, zero\n"
        "csrw 0x3A1, zero\n"
        "csrw 0x3A2, zero\n"
        "csrw 0x3A3, zero\n"
        "fence.i\n"
        "fence rw,rw\n"
        ::: "memory");
    const pmp_snapshot s = read_pmp();
    return s.cfg[0] == 0 && s.cfg[1] == 0 && s.cfg[2] == 0 && s.cfg[3] == 0;
#else
    return false; // host: 未対応。CSR を触っていないので成功扱いにしない
#endif
  }
};

namespace pma_c6 {
// 期待される PMA 16 組: entry 0-12 は IDF v6.1 の定数 (bit-exact 保存)、13-15 は本 layout。
struct expected_layout {
  uint32_t cfg[16];
  uint32_t addr[16];
};
constexpr expected_layout expected_all(uintptr_t iram_low, uintptr_t iram_text_end, uintptr_t dram_high) noexcept {
  expected_layout x{};
  for (uint32_t i = 0; i < IDF_INVALID_COUNT; ++i) {
    x.cfg[i] = IDF_INVALID_CFG[i];
    x.addr[i] = IDF_INVALID_ADDR[i];
  }
  const entry e13 = expected13(iram_low);
  const entry e14 = expected14(iram_text_end);
  const entry e15 = expected15(dram_high);
  x.cfg[13] = e13.cfg;
  x.addr[13] = e13.addr;
  x.cfg[14] = e14.cfg;
  x.addr[14] = e14.addr;
  x.cfg[15] = e15.cfg;
  x.addr[15] = e15.addr;
  return x;
}

// 書込み前の事前条件 (pure): entry 0-12 が IDF 定数と一致、entry 13-15 が cfg=0 かつ unlock。
// 1 つでも外れたら何も書かない (fail-closed)。
constexpr bool hp_mem_preconditions_ok(const uint32_t cfg[16], const uint32_t addr[16]) noexcept {
  for (uint32_t i = 0; i < IDF_INVALID_COUNT; ++i) {
    if (cfg[i] != IDF_INVALID_CFG[i] || addr[i] != IDF_INVALID_ADDR[i]) return false;
  }
  for (uint32_t i = 13; i < 16; ++i) {
    if (cfg[i] != 0 || cfg_locked(cfg[i])) return false;
  }
  return true;
}

// 読み戻した実値と期待値の完全一致 (pure)。pre-write の値や不一致の 1 語でも false になる (fail-closed)。
constexpr bool readback_matches(const uint32_t cfg_rb[16], const uint32_t addr_rb[16], const expected_layout &x) noexcept {
  for (uint32_t i = 0; i < 16; ++i) {
    if (cfg_rb[i] != x.cfg[i] || addr_rb[i] != x.addr[i]) return false;
  }
  return true;
}

#if defined(__riscv)
// PMA CSR を 16 entry 分読む (cfg 0xBC0-0xBCF、addr 0xBD0-0xBDF)。
inline void read_pma(uint32_t cfg[16], uint32_t addr[16]) noexcept {
  asm volatile("csrr %0, 0xBC0" : "=r"(cfg[0]));
  asm volatile("csrr %0, 0xBC1" : "=r"(cfg[1]));
  asm volatile("csrr %0, 0xBC2" : "=r"(cfg[2]));
  asm volatile("csrr %0, 0xBC3" : "=r"(cfg[3]));
  asm volatile("csrr %0, 0xBC4" : "=r"(cfg[4]));
  asm volatile("csrr %0, 0xBC5" : "=r"(cfg[5]));
  asm volatile("csrr %0, 0xBC6" : "=r"(cfg[6]));
  asm volatile("csrr %0, 0xBC7" : "=r"(cfg[7]));
  asm volatile("csrr %0, 0xBC8" : "=r"(cfg[8]));
  asm volatile("csrr %0, 0xBC9" : "=r"(cfg[9]));
  asm volatile("csrr %0, 0xBCA" : "=r"(cfg[10]));
  asm volatile("csrr %0, 0xBCB" : "=r"(cfg[11]));
  asm volatile("csrr %0, 0xBCC" : "=r"(cfg[12]));
  asm volatile("csrr %0, 0xBCD" : "=r"(cfg[13]));
  asm volatile("csrr %0, 0xBCE" : "=r"(cfg[14]));
  asm volatile("csrr %0, 0xBCF" : "=r"(cfg[15]));
  asm volatile("csrr %0, 0xBD0" : "=r"(addr[0]));
  asm volatile("csrr %0, 0xBD1" : "=r"(addr[1]));
  asm volatile("csrr %0, 0xBD2" : "=r"(addr[2]));
  asm volatile("csrr %0, 0xBD3" : "=r"(addr[3]));
  asm volatile("csrr %0, 0xBD4" : "=r"(addr[4]));
  asm volatile("csrr %0, 0xBD5" : "=r"(addr[5]));
  asm volatile("csrr %0, 0xBD6" : "=r"(addr[6]));
  asm volatile("csrr %0, 0xBD7" : "=r"(addr[7]));
  asm volatile("csrr %0, 0xBD8" : "=r"(addr[8]));
  asm volatile("csrr %0, 0xBD9" : "=r"(addr[9]));
  asm volatile("csrr %0, 0xBDA" : "=r"(addr[10]));
  asm volatile("csrr %0, 0xBDB" : "=r"(addr[11]));
  asm volatile("csrr %0, 0xBDC" : "=r"(addr[12]));
  asm volatile("csrr %0, 0xBDD" : "=r"(addr[13]));
  asm volatile("csrr %0, 0xBDE" : "=r"(addr[14]));
  asm volatile("csrr %0, 0xBDF" : "=r"(addr[15]));
}
#endif

// M-mode 専用: entry 13-15 を設定して、全 16 entry を書込み後に読み直す。
// 事前: entry 0-12 が IDF 定数と一致、entry 13-15 が unlock かつ cfg=0 (IDF の reset 値)。
// 書込み順: addr13 -> cfg13 -> addr14 -> cfg14 -> addr15 -> cfg15 (entry14 の LOCK が TOR 下限 addr13 を固定しうるため)。
// 戻り値 false: 非 RISC-V (未対応、何も書かない)、layout 不正、事前条件違反、読み戻し不一致のいずれか。
// 呼び出し側は U-mode に入らず停止する。
inline bool install_hp_mem_policy(uintptr_t iram_low, uintptr_t iram_text_end, uintptr_t dram_high) noexcept {
  if (!layout_valid(iram_low, iram_text_end, dram_high)) return false;
#if defined(__riscv)
  const expected_layout x = expected_all(iram_low, iram_text_end, dram_high);
  uint32_t pre_cfg[16] = {}, pre_addr[16] = {};
  read_pma(pre_cfg, pre_addr);
  if (!hp_mem_preconditions_ok(pre_cfg, pre_addr)) return false;
  const entry e13 = expected13(iram_low);
  const entry e14 = expected14(iram_text_end);
  const entry e15 = expected15(dram_high);
  asm volatile(
      "csrw 0xBDD, %0\n"
      "csrw 0xBCD, %1\n"
      "csrw 0xBDE, %2\n"
      "csrw 0xBCE, %3\n"
      "csrw 0xBDF, %4\n"
      "csrw 0xBCF, %5\n"
      "fence rw,rw\n"
      :
      : "r"(e13.addr), "r"(e13.cfg), "r"(e14.addr), "r"(e14.cfg), "r"(e15.addr), "r"(e15.cfg)
      : "memory");
  uint32_t post_cfg[16] = {}, post_addr[16] = {};
  read_pma(post_cfg, post_addr);
  return readback_matches(post_cfg, post_addr, x);
#else
  return false;  // host: CSR を触らない。成功を返さない (未対応)
#endif
}
}  // namespace pma_c6

} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_PMP_C6_HPP
