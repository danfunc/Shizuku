#include <cassert>
#include <cstdio>
#include "ports/esp32/c6/pmp.hpp"

using namespace shizuku::ports::c6;

static void test_pmp_napot_encoding() {
  pmp_entry entry;
  uint32_t decoded_base = 0;
  uint32_t decoded_size = 0;

  // 1. 最小サイズ: 8 bytes (k=3, mask=0)
  // base = 0x40800000 (8B 整列)
  // expected pmpaddr = (0x40800000 >> 2) | 0 = 0x10200000
  // expected cfg = (NAPOT << 3) | (R | X) = (3 << 3) | 5 = 24 | 5 = 29 (0x1D)
  bool ok = pmp_codec::encode_napot(0x40800000, 8, pmp_perm::R | pmp_perm::X, entry);
  assert(ok);
  assert(entry.addr == 0x10200000u);
  assert(entry.cfg == (static_cast<uint8_t>(pmp_mode::NAPOT) << 3 | pmp_perm::R | pmp_perm::X));
  ok = pmp_codec::decode_napot(entry, decoded_base, decoded_size);
  assert(ok);
  assert(decoded_base == 0x40800000u);
  assert(decoded_size == 8u);

  // 2. 16 bytes (k=4, mask=1)
  // base = 0x40800010 (16B 整列)
  // expected pmpaddr = (0x40800010 >> 2) | 1 = 0x10200004 | 1 = 0x10200005
  ok = pmp_codec::encode_napot(0x40800010, 16, pmp_perm::R | pmp_perm::W, entry);
  assert(ok);
  assert(entry.addr == 0x10200005u);
  ok = pmp_codec::decode_napot(entry, decoded_base, decoded_size);
  assert(ok);
  assert(decoded_base == 0x40800010u);
  assert(decoded_size == 16u);

  // 3. 4096 bytes (4KiB, k=12, mask=(512-1)=511=0x1FF)
  // base = 0x40801000
  // base >> 2 = 0x10200400
  // expected pmpaddr = 0x10200400 | 0x1FF = 0x102005FF
  ok = pmp_codec::encode_napot(0x40801000, 4096, pmp_perm::R | pmp_perm::W | pmp_perm::X, entry);
  assert(ok);
  assert(entry.addr == 0x102005FFu);
  ok = pmp_codec::decode_napot(entry, decoded_base, decoded_size);
  assert(ok);
  assert(decoded_base == 0x40801000u);
  assert(decoded_size == 4096u);

  // 4. 64KiB (65536 bytes, k=16, mask=(8192-1)=8191=0x1FFF)
  // base = 0x40810000
  // base >> 2 = 0x10204000
  // expected pmpaddr = 0x10204000 | 0x1FFF = 0x10205FFFu
  ok = pmp_codec::encode_napot(0x40810000, 65536, pmp_perm::R | pmp_perm::L, entry);
  assert(ok);
  assert(entry.addr == 0x10205FFFu);
  assert((entry.cfg & pmp_perm::L) != 0);
  ok = pmp_codec::decode_napot(entry, decoded_base, decoded_size);
  assert(ok);
  assert(decoded_base == 0x40810000u);
  assert(decoded_size == 65536u);
  // 5. 4GB NAPOT (ESP32-C6 TRM 最大範囲: 0x100000000ULL, base=0, k=32, mask=(2^29 - 1)=0x1FFFFFFF)
  // expected pmpaddr = 0 | 0x1FFFFFFF = 0x1FFFFFFF
  uint64_t decoded_size64 = 0;
  ok = pmp_codec::encode_napot(0x00000000, 0x100000000ULL, pmp_perm::R | pmp_perm::X, entry);
  assert(ok);
  assert(entry.addr == 0x1FFFFFFFu);
  ok = pmp_codec::decode_napot(entry, decoded_base, decoded_size64);
  assert(ok);
  assert(decoded_base == 0x00000000u);
  assert(decoded_size64 == 0x100000000ULL);

  // 32-bit decode_napot では 4GB のオーバーフローを検知して安全に false を返す (能力限界)
  assert(!pmp_codec::decode_napot(entry, decoded_base, decoded_size));
}

static void test_pmp_napot_rejection() {
  pmp_entry entry;

  // 1. サイズ < 8 の拒否 (NAPOT は 8B 以上)
  assert(!pmp_codec::encode_napot(0x40800000, 4, pmp_perm::R, entry));
  assert(!pmp_codec::encode_napot(0x40800000, 0, pmp_perm::R, entry));

  // 2. 2 のべき乗でないサイズの拒否
  assert(!pmp_codec::encode_napot(0x40800000, 12, pmp_perm::R, entry));
  assert(!pmp_codec::encode_napot(0x40800000, 1000, pmp_perm::R, entry));
  assert(!pmp_codec::encode_napot(0x40800000, 3000, pmp_perm::R, entry));

  // 3. base アドレスがサイズ整列していない場合の拒否
  assert(!pmp_codec::encode_napot(0x40800004, 8, pmp_perm::R, entry));
  assert(!pmp_codec::encode_napot(0x40800008, 16, pmp_perm::R, entry));
  assert(!pmp_codec::encode_napot(0x40800800, 4096, pmp_perm::R, entry));

  // 4. 不正なパーミッションビットの拒否 (未知ビット)
  assert(!pmp_codec::encode_napot(0x40800000, 8, 0x10, entry));
  assert(!pmp_codec::encode_napot(0x40800000, 8, 0x40, entry));

  // 5. RISC-V PMP 予約済み (reserved) の拒否: W=1 かつ R=0
  // Write-only (W=1, R=0, X=0)
  assert(!pmp_codec::encode_napot(0x40800000, 8, pmp_perm::W, entry));
  // Write-Execute without Read (W=1, R=0, X=1)
  assert(!pmp_codec::encode_napot(0x40800000, 8, pmp_perm::W | pmp_perm::X, entry));
}

static void test_pmp_tor() {
  pmp_entry entry;
  uint32_t decoded_limit = 0;

  // limit = 0x40804000
  // expected pmpaddr = 0x40804000 >> 2 = 0x10201000
  bool ok = pmp_codec::encode_tor(0x40804000, pmp_perm::R | pmp_perm::X, entry);
  assert(ok);
  assert(entry.addr == 0x10201000u);
  assert(entry.cfg == (static_cast<uint8_t>(pmp_mode::TOR) << 3 | pmp_perm::R | pmp_perm::X));
  ok = pmp_codec::decode_tor(entry, decoded_limit);
  assert(ok);
  assert(decoded_limit == 0x40804000u);

  // 非 4B 整列の拒否
  assert(!pmp_codec::encode_tor(0x40804001, pmp_perm::R, entry));
  assert(!pmp_codec::encode_tor(0x40804002, pmp_perm::R, entry));
  assert(!pmp_codec::encode_tor(0x40804003, pmp_perm::R, entry));

  // TOR における W=1, R=0 (reserved) の拒否
  assert(!pmp_codec::encode_tor(0x40804000, pmp_perm::W, entry));
  assert(!pmp_codec::encode_tor(0x40804000, pmp_perm::W | pmp_perm::X, entry));
}

static void test_pmpcfg_packing() {
  uint8_t c0 = 0x1F;
  uint8_t c1 = 0x09;
  uint8_t c2 = 0x9B;
  uint8_t c3 = 0x00;

  uint32_t packed = pmp_codec::pack_pmpcfg(c0, c1, c2, c3);
  assert(pmp_codec::unpack_pmpcfg(packed, 0) == c0);
  assert(pmp_codec::unpack_pmpcfg(packed, 1) == c1);
  assert(pmp_codec::unpack_pmpcfg(packed, 2) == c2);
  assert(pmp_codec::unpack_pmpcfg(packed, 3) == c3);
}

int main() {
  printf("[TEST] Running PMP unit tests...\n");
  test_pmp_napot_encoding();
  printf("[TEST]   PMP NAPOT encoding/decoding passed.\n");
  test_pmp_napot_rejection();
  printf("[TEST]   PMP NAPOT rejection cases passed.\n");
  test_pmp_tor();
  printf("[TEST]   PMP TOR encoding/decoding passed.\n");
  test_pmpcfg_packing();
  printf("[TEST]   PMP CSR configuration packing passed.\n");
  printf("[TEST] All PMP unit tests successfully PASSED!\n");
  return 0;
}
