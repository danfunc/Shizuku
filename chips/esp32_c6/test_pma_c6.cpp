// 段1: PMA entry 0-15 の期待値・境界・読み戻し比較 (pure)、事前条件 (pure)、PMP の lock 検査、host の未対応返却のホスト単体テスト。
// ★host では CSR を一切触らない。install/apply/disable は host で必ず false を返す (成功扱いにしない)。
// 期待値の出典: TRM v1.2 §1.9.5 p.69 bit map (controller 確認)、ESP-IDF v6.1 invalid-region 定数 (r6 ELF 逆アセンブル、controller 確認の公式ソース行 28-61)。
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "chips/esp32_c6/pmp_c6.hpp"

using namespace shizuku::chips::esp32_c6;

int main() {
  // 1. 定数: TRM bit map と IDF 定数 (deny=0x60000001, OFF=0x20000001)
  static_assert(pma_c6::IDF_INVALID_COUNT == 13, "IDF invalid region count is 13");
  static_assert(sizeof(pma_c6::IDF_INVALID_ADDR) / sizeof(uint32_t) == 13, "address table length");
  static_assert(sizeof(pma_c6::IDF_INVALID_CFG) / sizeof(uint32_t) == 13, "cfg table length");
  assert((pma_c6::CFG_A_TOR | pma_c6::CFG_LOCK | pma_c6::CFG_VALID) == 0x60000001u);
  assert((pma_c6::CFG_LOCK | pma_c6::CFG_VALID) == 0x20000001u);
  assert(pma_c6::IDF_INVALID_CFG[0] == 0x60000001u && pma_c6::IDF_INVALID_CFG[1] == 0x20000001u);
  assert(pma_c6::IDF_INVALID_ADDR[0] == 0x08000000u && pma_c6::IDF_INVALID_ADDR[12] == 0x3FFFFFFFu);
  std::printf("PASS: TRM bit map and IDF invalid-region constants\n");

  // 2. 期待 entry 13-15 (IRAM [0x40800000,0x40809788), DRAM [0x40809788,0x40880000))
  const uintptr_t iram_low = 0x40800000u;
  const uintptr_t iram_text_end = 0x40809788u;  // = r6 map _iram_text_end; 8B .iram0.data は DRAM 規則側
  const uintptr_t dram_high = 0x40880000u;
  const pma_c6::entry e13 = pma_c6::expected13(iram_low);
  const pma_c6::entry e14 = pma_c6::expected14(iram_text_end);
  const pma_c6::entry e15 = pma_c6::expected15(dram_high);
  assert(e13.cfg == 0x20000000u && e13.addr == 0x10200000u);  // OFF + LOCK、addr13 = IRAM low
  assert(e14.cfg == 0x60000015u && e14.addr == 0x102025E2u);  // TOR + LOCK + R|X + valid
  assert(e15.cfg == 0x60000019u && e15.addr == 0x10220000u);  // TOR + LOCK + R|W + valid
  assert((e14.cfg & pma_c6::CFG_W) == 0 && (e14.cfg & pma_c6::CFG_X) != 0);  // IRAM: W 無し
  assert((e15.cfg & pma_c6::CFG_X) == 0 && (e15.cfg & pma_c6::CFG_W) != 0);  // DRAM: X 無し
  std::printf("PASS: expected entry13/14/15 encodings\n");

  // 3. expected_all: 0-12 は IDF 定数と bit-exact、13-15 は上の期待値
  const pma_c6::expected_layout x = pma_c6::expected_all(iram_low, iram_text_end, dram_high);
  for (uint32_t i = 0; i < 13; ++i) {
    assert(x.cfg[i] == pma_c6::IDF_INVALID_CFG[i] && x.addr[i] == pma_c6::IDF_INVALID_ADDR[i]);
  }
  assert(x.cfg[13] == e13.cfg && x.addr[13] == e13.addr);
  assert(x.cfg[14] == e14.cfg && x.addr[14] == e14.addr);
  assert(x.cfg[15] == e15.cfg && x.addr[15] == e15.addr);
  std::printf("PASS: expected_all preserves IDF entries 0-12 and sets 13-15\n");

  // 4. readback_matches: 正例は完全一致のみ。stale (書込み前の 0)、アドレス 1 語の違い、LOCK 欠落、IDF 領域のずれは false
  uint32_t cfg_rb[16], addr_rb[16];
  for (uint32_t i = 0; i < 16; ++i) { cfg_rb[i] = x.cfg[i]; addr_rb[i] = x.addr[i]; }
  assert(pma_c6::readback_matches(cfg_rb, addr_rb, x));  // 正例: 16 語すべて一致
  cfg_rb[13] = 0;  // stale: 書込み前の entry13 cfg (OFF、LOCK 無し)
  assert(!pma_c6::readback_matches(cfg_rb, addr_rb, x));
  cfg_rb[13] = x.cfg[13];
  addr_rb[15] = x.addr[15] + 1u;  // 1 語のアドレス違い
  assert(!pma_c6::readback_matches(cfg_rb, addr_rb, x));
  addr_rb[15] = x.addr[15];
  cfg_rb[14] = 0x60000005u;  // LOCK 欠落 (期待 0x60000015): 書込みが lock されていない
  assert(!pma_c6::readback_matches(cfg_rb, addr_rb, x));
  cfg_rb[14] = x.cfg[14];
  cfg_rb[3] ^= 0x1u;  // IDF 領域 (0-12) の 1 bit のずれ
  assert(!pma_c6::readback_matches(cfg_rb, addr_rb, x));
  cfg_rb[3] = x.cfg[3];
  cfg_rb[0] = x.cfg[0] & ~pma_c6::CFG_LOCK;  // IDF 領域 entry 0 の LOCK が落ちている
  assert(!pma_c6::readback_matches(cfg_rb, addr_rb, x));
  cfg_rb[0] = x.cfg[0];
  assert(pma_c6::readback_matches(cfg_rb, addr_rb, x));  // 元に戻すと再び一致
  std::printf("PASS: readback_matches exact, stale, lock-missing, and IDF-region mismatch cases\n");

  // 5. hp_mem_preconditions_ok (書込み前): 0-12 は IDF 定数、13-15 は cfg=0 かつ unlock
  uint32_t pre_cfg[16], pre_addr[16];
  for (uint32_t i = 0; i < 16; ++i) { pre_cfg[i] = 0; pre_addr[i] = 0; }
  for (uint32_t i = 0; i < 13; ++i) { pre_cfg[i] = pma_c6::IDF_INVALID_CFG[i]; pre_addr[i] = pma_c6::IDF_INVALID_ADDR[i]; }
  assert(pma_c6::hp_mem_preconditions_ok(pre_cfg, pre_addr));  // 正例: IDF の reset 状態
  pre_cfg[13] = pma_c6::CFG_LOCK;  // entry13 が既に LOCK (OFF+LOCK)
  assert(!pma_c6::hp_mem_preconditions_ok(pre_cfg, pre_addr));
  pre_cfg[13] = 0;
  pre_cfg[15] = pma_c6::CFG_VALID;  // entry15 に既に設定がある
  assert(!pma_c6::hp_mem_preconditions_ok(pre_cfg, pre_addr));
  pre_cfg[15] = 0;
  pre_cfg[5] ^= 0x1u;  // IDF 領域 entry5 のずれ
  assert(!pma_c6::hp_mem_preconditions_ok(pre_cfg, pre_addr));
  std::printf("PASS: hp_mem_preconditions_ok positive and lock/mismatch rejection\n");

  // 6. 境界 (layout_valid): 正例と境界ぎわ、負例
  assert(pma_c6::layout_valid(iram_low, iram_text_end, dram_high));
  assert(pma_c6::layout_valid(pma_c6::HP_MEM_LOW, pma_c6::HP_MEM_LOW + 4u, pma_c6::HP_MEM_HIGH));  // 最小 IRAM
  assert(pma_c6::layout_valid(pma_c6::HP_MEM_LOW, pma_c6::HP_MEM_HIGH - 4u, pma_c6::HP_MEM_HIGH));  // 最大 IRAM
  assert(!pma_c6::layout_valid(pma_c6::HP_MEM_LOW, pma_c6::HP_MEM_LOW, pma_c6::HP_MEM_HIGH));  // 空 IRAM
  assert(!pma_c6::layout_valid(pma_c6::HP_MEM_LOW, pma_c6::HP_MEM_HIGH, pma_c6::HP_MEM_HIGH));  // 空 DRAM
  assert(!pma_c6::layout_valid(pma_c6::HP_MEM_LOW - 4u, iram_text_end, dram_high));  // HP_MEM 外 (下)
  assert(!pma_c6::layout_valid(iram_low, iram_text_end, pma_c6::HP_MEM_HIGH + 4u));  // HP_MEM 外 (上)
  assert(!pma_c6::layout_valid(iram_low, 0x40809789u, dram_high));  // 非整列
  assert(!pma_c6::layout_valid(0x40050000u, iram_text_end, dram_high));  // IDF deny [0x40050000,0x40800000) と重なる
  assert(!pma_c6::layout_valid(0x40000000u, iram_text_end, dram_high));  // ROM は HP_MEM ではない
  assert(!pma_c6::ranges_overlap(0x0u, 4u, 4u, 8u));  // 隣接は重ならない (半開)
  assert(pma_c6::ranges_overlap(0x0u, 5u, 4u, 8u));   // 1 バイトの重なり
  std::printf("PASS: layout_valid boundaries and negative cases\n");

  // 7. PMP lock 検査 (pure): 1 bit でも L があれば所有権を取らない
  const uint32_t unlocked[4] = {0x00000000u, 0x7F7F7F7Fu, 0x00000009u, 0x00001F1Fu};
  const uint32_t locked0[4] = {0x00000080u, 0, 0, 0};
  const uint32_t locked3[4] = {0, 0, 0, 0x80000000u};
  assert(pma_c6::cfg_locked(0x20000000u) && !pma_c6::cfg_locked(0x40000001u));
  assert(pmp_cfg_all_unlocked(unlocked));
  assert(!pmp_cfg_all_unlocked(locked0));
  assert(!pmp_cfg_all_unlocked(locked3));
  std::printf("PASS: PMP locked-state predicates\n");

  // 8. host は CSR を触らず未対応として false (成功扱いしない)
  pmp_c6_config cfg{};
  cfg.code_base = 0x42000000u;
  cfg.code_limit = 0x42010000u;
  cfg.stack_base = 0x40820000u;
  cfg.stack_limit = 0x40821000u;
  assert(!pmp_controller::apply(cfg, 0x40841000u, 4096u, 0u, 0u));
  assert(!pmp_controller::disable());
  assert(!pma_c6::install_hp_mem_policy(iram_low, iram_text_end, dram_high));
  assert(!pma_c6::install_hp_mem_policy(iram_low, 0x40809789u, dram_high));
  std::printf("PASS: host hardware APIs return unsupported (false)\n");

  // 9. PMA ready marker (pure): 0 (未設定) や別の値は U へ入る根拠にならない。READY_MAGIC のみ true。
  assert(!pma_c6::ready_marker_ok(0u));                       // 未設定 (IDF が呼ばれなかった場合)
  assert(pma_c6::ready_marker_ok(pma_c6::READY_MAGIC));       // 正しい値
  assert(!pma_c6::ready_marker_ok(pma_c6::READY_MAGIC ^ 1u)); // 1 bit 違い
  assert(!pma_c6::ready_marker_ok(0xFFFFFFFFu));              // 不定値
  assert(pma_c6::READY_MAGIC == 0x5A1F0A11u);                 // C wrapper の定義と一致させる (test_trap_static.py も比較)
  std::printf("PASS: PMA ready marker predicate (0 / wrong / exact)\n");

  std::printf("ALL PASS: test_pma_c6\n");
  return 0;
}
