#ifndef SHIZUKU_PORTS_ESP32_C6_PMP_HPP
#define SHIZUKU_PORTS_ESP32_C6_PMP_HPP

#include <cstddef>
#include <cstdint>

namespace shizuku {
namespace ports {
namespace c6 {

// ESP32-C6 TRM PMP 幾何・エンコード仕様 (TRM cites RISC-V Privileged Architecture v1.10).
// ESP32-C6 は 16 個の PMP エントリ (pmpcfg0..pmpcfg3, pmpaddr0..pmpaddr15) を持ち、
// M-mode から設定して U-mode オブジェクトのメモリ保護 (R, W, X) を行う。
//
// 本モジュールはハードウェアレジスタへの直接アクセスを含まない純粋ロジック層であり、
// ホスト環境での単体テストおよびカーネル初期化ルーチン双方から安全に利用できる。

enum struct pmp_mode : uint8_t {
  OFF = 0,
  TOR = 1,
  NA4 = 2,
  NAPOT = 3,
};

namespace pmp_perm {
static constexpr uint8_t NONE = 0;
static constexpr uint8_t R = 1u << 0;
static constexpr uint8_t W = 1u << 1;
static constexpr uint8_t X = 1u << 2;
static constexpr uint8_t L = 1u << 7; // Lock bit
} // namespace pmp_perm

struct pmp_entry {
  uint8_t cfg = 0;
  uint32_t addr = 0;
};

class pmp_codec {
public:
  static constexpr uint8_t MODE_MASK = 0x18u; // bits 3..4
  static constexpr uint8_t PERM_MASK = 0x87u; // R, W, X, L

  // ---- NAPOT (Naturally Aligned Power-of-Two) -----------------------------
  // 対象サイズ: 2^k バイト (k >= 3, 即ち 8 バイト以上)。
  // pmpaddr エンコーディング規則:
  //   size = 2^k のとき、下位 (k - 3) ビットが 1 で、bit (k - 3) が 0 となる。
  //   pmpaddr = (base >> 2) | ((size >> 3) - 1)
  //   ESP32-C6 TRM / RISC-V Privileged spec v1.10: 最小 NAPOT は 8B、最大は 4GB。
  static constexpr bool encode_napot(uint32_t base, uint64_t size, uint8_t perm,
                                     pmp_entry &out) noexcept {
    if (size < 8 || size > 0x100000000ULL) {
      return false; // NAPOT は 8 バイト以上、最大 4GB (2^32) まで
    }
    if ((size & (size - 1)) != 0) {
      return false; // 2 のべき乗でないサイズは表現不可
    }
    if ((static_cast<uint64_t>(base) & (size - 1)) != 0) {
      return false; // 自然境界整列でない base は表現不可
    }
    if ((perm & ~PERM_MASK) != 0) {
      return false; // 未知・不正なパーミッションビット
    }
    // RISC-V PMP 仕様 (v1.10 3.7節): W=1 かつ R=0 は将来標準用に予約 (reserved)
    if ((perm & pmp_perm::W) != 0 && (perm & pmp_perm::R) == 0) {
      return false;
    }

    const uint32_t mask = static_cast<uint32_t>((size >> 3) - 1u);
    out.addr = (base >> 2) | mask;
    out.cfg = perm | (static_cast<uint8_t>(pmp_mode::NAPOT) << 3);
    return true;
  }

  static constexpr bool decode_napot(const pmp_entry &entry, uint32_t &out_base,
                                     uint64_t &out_size) noexcept {
    const uint8_t mode = (entry.cfg >> 3) & 0x3u;
    if (mode != static_cast<uint8_t>(pmp_mode::NAPOT)) {
      return false;
    }

    // pmpaddr の下位から連続する 1 のビット数を数える
    uint32_t val = entry.addr;
    uint32_t trailing_ones = 0;
    while ((val & 1u) != 0) {
      trailing_ones++;
      val >>= 1;
    }

    // 32-bit アドレス空間上限: trailing_ones=29 のとき 8 << 29 = 4GiB (全アドレス空間)
    if (trailing_ones > 29) {
      return false;
    }

    // uint64_t で計算し、32bit シフト未定義動作 (8u << 29) を防止
    out_size = static_cast<uint64_t>(8) << trailing_ones;
    const uint32_t mask = (1u << trailing_ones) - 1u;
    out_base = (entry.addr ^ mask) << 2;
    return true;
  }

  // 32-bit サイズ用の decode_napot: 4GB の場合は uint32_t に収まらないため
  // 表現能力限界として安全に false を返し、overflow/未定義動作を防ぐ。
  static constexpr bool decode_napot(const pmp_entry &entry, uint32_t &out_base,
                                     uint32_t &out_size) noexcept {
    uint64_t size64 = 0;
    if (!decode_napot(entry, out_base, size64)) {
      return false;
    }
    if (size64 > 0xFFFFFFFFULL) {
      return false; // 4GB は uint32_t で表現不可
    }
    out_size = static_cast<uint32_t>(size64);
    return true;
  }

  // ---- TOR (Top of Range) -------------------------------------------------
  // 範囲: pmpaddr[i-1] <= addr < pmpaddr[i]
  // 境界アドレスは 4 バイト境界であること。
  static constexpr bool encode_tor(uint32_t limit_addr, uint8_t perm,
                                   pmp_entry &out) noexcept {
    if ((limit_addr & 3u) != 0) {
      return false; // 4 バイト境界整列必須
    }
    if ((perm & ~PERM_MASK) != 0) {
      return false;
    }
    // RISC-V PMP 仕様 (v1.10 3.7節): W=1 かつ R=0 は予約 (reserved)
    if ((perm & pmp_perm::W) != 0 && (perm & pmp_perm::R) == 0) {
      return false;
    }

    out.addr = limit_addr >> 2;
    out.cfg = perm | (static_cast<uint8_t>(pmp_mode::TOR) << 3);
    return true;
  }

  static constexpr bool decode_tor(const pmp_entry &entry,
                                   uint32_t &out_limit) noexcept {
    const uint8_t mode = (entry.cfg >> 3) & 0x3u;
    if (mode != static_cast<uint8_t>(pmp_mode::TOR)) {
      return false;
    }

    out_limit = entry.addr << 2;
    return true;
  }

  // ---- pmpcfg CSR pack / unpack (4 entries per 32-bit CSR) ----------------
  static constexpr uint32_t pack_pmpcfg(uint8_t cfg0, uint8_t cfg1, uint8_t cfg2,
                                        uint8_t cfg3) noexcept {
    return static_cast<uint32_t>(cfg0) |
           (static_cast<uint32_t>(cfg1) << 8) |
           (static_cast<uint32_t>(cfg2) << 16) |
           (static_cast<uint32_t>(cfg3) << 24);
  }

  static constexpr uint8_t unpack_pmpcfg(uint32_t pmpcfg, unsigned slot) noexcept {
    return static_cast<uint8_t>((pmpcfg >> ((slot & 3u) * 8)) & 0xFFu);
  }
};

} // namespace c6
} // namespace ports
} // namespace shizuku

#endif // SHIZUKU_PORTS_ESP32_C6_PMP_HPP
