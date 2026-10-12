#ifndef SHIZUKU_ARCH_RISCV_PMP_HPP
#define SHIZUKU_ARCH_RISCV_PMP_HPP

#include <cstddef>
#include <cstdint>

namespace shizuku {
namespace ports {
namespace riscv {

// RISC-V PMP (Privileged Architecture v1.10) のエンコード仕様。
// エントリ数・レジスタ配置はチップ依存なのでここには書かない (chips 側で定める)。
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

// GranuleBytes: チップが実装する PMP の粒度 (バイト単位。4 または 128 など)。
// spec の cfg.G フィールド (2^(G+2) バイト) とは別物で、ここでは直接バイト数で渡す。
//   - 4   : 仕様上の最小 (NAPOT 最小 8B、TOR は 4B 整列)
//   - 128 : 粗い粒度の実装 (NAPOT 最小 128B、TOR 境界も 128B 整列)
// 物理エントリ数 (N) は含めない。チップ側が決める。
template <uint32_t GranuleBytes> class pmp_codec {
  static_assert(GranuleBytes >= 4 && (GranuleBytes & (GranuleBytes - 1)) == 0,
                "PMP granule must be a power of two and at least 4 bytes");

public:
  static constexpr uint8_t MODE_MASK = 0x18u; // bits 3..4
  static constexpr uint8_t PERM_MASK = 0x87u; // R, W, X, L
  static constexpr uint32_t GRANULE = GranuleBytes;
  // NAPOT の最小サイズ。仕様上の下限 8B と粒度の大きい方。
  static constexpr uint64_t NAPOT_MIN_BYTES = GranuleBytes > 8 ? GranuleBytes : 8;

  // ---- NAPOT (Naturally Aligned Power-of-Two) -----------------------------
  // 対象サイズ: 2^k バイト (k >= 3, 即ち 8 バイト以上、粒度以上)。
  // pmpaddr エンコーディング規則:
  //   size = 2^k のとき、下位 (k - 3) ビットが 1 で、bit (k - 3) が 0 となる。
  //   pmpaddr = (base >> 2) | ((size >> 3) - 1)
  //   RISC-V Privileged spec v1.10: 最小 NAPOT は 8B、最大は 4GB (32bit アドレス空間)。
  //   G=128 のときは最小 128B (粒度) に引き上げる。
  static constexpr bool encode_napot(uint32_t base, uint64_t size, uint8_t perm,
                                     pmp_entry &out) noexcept {
    if (size < NAPOT_MIN_BYTES || size > 0x100000000ULL) {
      return false; // NAPOT は最小サイズ以上、最大 4GB (2^32) まで
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
    if (out_size < NAPOT_MIN_BYTES) {
      return false; // 粒度に満たないエントリはこの実装では表現しない
    }
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
  // 境界アドレスは粒度 (GranuleBytes) の整列であること (G=4 なら 4 バイト整列)。
  static constexpr bool encode_tor(uint32_t limit_addr, uint8_t perm,
                                   pmp_entry &out) noexcept {
    if ((limit_addr & (GranuleBytes - 1u)) != 0) {
      return false; // 粒度整列必須
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

} // namespace riscv
} // namespace ports
} // namespace shizuku

#endif // SHIZUKU_ARCH_RISCV_PMP_HPP
