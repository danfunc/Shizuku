// ===========================================================================
//  レジスタ漏洩防止試験 (Register Leakage Prevention Test)
// ===========================================================================
//  非特権の呼び手 (Caller) と呼び先 (Callee) の間で、不要なレジスタ値が漏洩しない
//  こと、および呼び手の callee-saved レジスタが元通り復元されることを検証する。
//  - 試験 A: 呼び手 -> 呼び先 (Caller の callee-saved が呼び先に見えないこと)
//  - 試験 B1: 呼び先 -> 呼び手 (Callee の一時レジスタ値が呼び手に漏洩しないこと)
//  - 試験 B2: 呼び手の callee-saved 復元 (呼び出し前の値に完全に復元されること)

#include <cstdint>
#include <cstddef>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"

alignas(32) uint8_t leak_caller_data[256];
alignas(32) uint8_t leak_callee_data[256];

extern "C" {
uintptr_t leak_caller_worker(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t leak_callee_worker(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
}

#if defined(__ARM_ARCH_8M_MAIN__)
__attribute__((naked)) uintptr_t leak_callee_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      // 入口直後に r4-r11, r12 の値を退避
      "push {r0, r1}\n"
      "ldr  r0, =leak_callee_data\n"
      "str  r4, [r0, #0]\n"
      "str  r5, [r0, #4]\n"
      "str  r6, [r0, #8]\n"
      "str  r7, [r0, #12]\n"
      "str  r8, [r0, #16]\n"
      "str  r9, [r0, #20]\n"
      "str  r10, [r0, #24]\n"
      "str  r11, [r0, #28]\n"
      "str  r12, [r0, #32]\n"
      "pop  {r0, r1}\n"

      // 戻る前に全レジスタに 0xCAFE0000 | reg をセット (戻り値 r0 以外)
      "movw r2, #0x0002\n"
      "movt r2, #0xCAFE\n"
      "movw r3, #0x0003\n"
      "movt r3, #0xCAFE\n"
      "movw r4, #0x0004\n"
      "movt r4, #0xCAFE\n"
      "movw r5, #0x0005\n"
      "movt r5, #0xCAFE\n"
      "movw r6, #0x0006\n"
      "movt r6, #0xCAFE\n"
      "movw r7, #0x0007\n"
      "movt r7, #0xCAFE\n"
      "movw r8, #0x0008\n"
      "movt r8, #0xCAFE\n"
      "movw r9, #0x0009\n"
      "movt r9, #0xCAFE\n"
      "movw r10, #0x000A\n"
      "movt r10, #0xCAFE\n"
      "movw r11, #0x000B\n"
      "movt r11, #0xCAFE\n"
      "movw r12, #0x000C\n"
      "movt r12, #0xCAFE\n"

      // 戻り値 r0 = 42
      "movs r0, #42\n"
      "bx   lr\n"
      ".ltorg\n"
  );
}

__attribute__((naked)) uintptr_t leak_caller_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  asm volatile(
      ".syntax unified\n"
      ".thumb\n"
      // Caller は呼び出し前に callee-saved (r4-r11) に 0xDEAD0000 | reg をセット
      "push {r4-r7, lr}\n"
      // leak_caller_data[0] から callee_id を取得
      "ldr  r0, =leak_caller_data\n"
      "ldr  r1, [r0, #0]\n"   // callee_id

      "movw r4, #0x0004\n"
      "movt r4, #0xDEAD\n"
      "movw r5, #0x0005\n"
      "movt r5, #0xDEAD\n"
      "movw r6, #0x0006\n"
      "movt r6, #0xDEAD\n"
      "movw r7, #0x0007\n"
      "movt r7, #0xDEAD\n"
      "movw r8, #0x0008\n"
      "movt r8, #0xDEAD\n"
      "movw r9, #0x0009\n"
      "movt r9, #0xDEAD\n"
      "movw r10, #0x000A\n"
      "movt r10, #0xDEAD\n"
      "movw r11, #0x000B\n"
      "movt r11, #0xDEAD\n"

      // CALL_METHOD(OBJECT_LEAK_CALLEE, 0, 99)
      "movs r0, #4\n"                 // CALL_METHOD = 4
      "movs r2, #0\n"                 // method 0
      "movs r3, #99\n"                // arg 99
      "mov  r12, #0\n"
      "svc  0\n"

      // 復帰直後: r0=err, r1=val
      // レジスタ値を leak_caller_data (offset 4 以降) に回収
      "push {r0, r1}\n"
      "ldr  r0, =leak_caller_data\n"
      "str  r4, [r0, #4]\n"   // restored_r4
      "str  r5, [r0, #8]\n"   // restored_r5
      "str  r6, [r0, #12]\n"  // restored_r6
      "str  r7, [r0, #16]\n"  // restored_r7
      "str  r8, [r0, #20]\n"  // restored_r8
      "str  r9, [r0, #24]\n"  // restored_r9
      "str  r10, [r0, #28]\n" // restored_r10
      "str  r11, [r0, #32]\n" // restored_r11
      "str  r2, [r0, #36]\n"  // restored_r2
      "str  r3, [r0, #40]\n"  // restored_r3
      "str  r12, [r0, #44]\n" // restored_r12
      "pop  {r0, r1}\n"
      "ldr  r2, =leak_caller_data\n"
      "str  r0, [r2, #48]\n"  // err
      "str  r1, [r2, #52]\n"  // val

      "pop  {r4-r7, pc}\n"
      ".ltorg\n"
  );
}
#else
extern "C" {
uintptr_t leak_caller_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t leak_callee_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
}
#endif

namespace shizuku::selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_LEAK_CALLER = object_id::leak_caller;
constexpr uintptr_t OBJECT_LEAK_CALLEE = object_id::leak_callee;

void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s\n", name);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void register_leak_test() {
#if defined(__ARM_ARCH_8M_MAIN__) || defined(SHIZUKU_RP2350)
  // 1. 保護窓を登録
  if constexpr (requires { ARCH::set_object_bounds(0, 0, 0, 0, 0); }) {
    ARCH::set_object_bounds(OBJECT_LEAK_CALLER,
                            reinterpret_cast<uintptr_t>(leak_caller_data),
                            reinterpret_cast<uintptr_t>(leak_caller_data) + sizeof(leak_caller_data),
                            0, 0);
    ARCH::set_object_bounds(OBJECT_LEAK_CALLEE,
                            reinterpret_cast<uintptr_t>(leak_callee_data),
                            reinterpret_cast<uintptr_t>(leak_callee_data) + sizeof(leak_callee_data),
                            0, 0);
  }

  // バッファの初期化
  auto *callee_seen = reinterpret_cast<volatile uint32_t *>(leak_callee_data);
  for (size_t i = 0; i < sizeof(leak_callee_data) / sizeof(uint32_t); ++i) {
    callee_seen[i] = 0;
  }
  auto *caller_data = reinterpret_cast<volatile uint32_t *>(leak_caller_data);
  for (size_t i = 0; i < sizeof(leak_caller_data) / sizeof(uint32_t); ++i) {
    caller_data[i] = 0;
  }
  caller_data[0] = OBJECT_LEAK_CALLEE;

  // 2. Caller と Callee を CREATE_OBJECT (非特権 OBJECT_UNPRIVILEGED=2)
  const auto create_callee_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CREATE_OBJECT),
      OBJECT_LEAK_CALLEE,
      reinterpret_cast<uintptr_t>(&leak_callee_worker),
      OBJECT_UNPRIVILEGED);
  if (create_callee_res.error != 0) {
    BOARD::panic("register_leak_test: failed to create callee");
  }

  const auto create_caller_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CREATE_OBJECT),
      OBJECT_LEAK_CALLER,
      reinterpret_cast<uintptr_t>(&leak_caller_worker),
      OBJECT_UNPRIVILEGED);
  if (create_caller_res.error != 0) {
    BOARD::panic("register_leak_test: failed to create caller");
  }

  // 3. Caller を呼ぶ (Caller が内部で Callee を呼ぶ)
  const auto call_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_LEAK_CALLER,
      0,
      0);

  // 4. アサーション検証
  // 試験 A: 呼び手から呼び先への漏洩防止 (Caller -> Callee)
  // Callee 入口で、Caller のカナリア 0xDEAD0004..0xDEAD000B が見えていないこと (消去されていること)
  bool leak_to_callee = false;
  for (int i = 0; i < 8; ++i) {
    if (callee_seen[i] == (0xDEAD0004u + i)) {
      leak_to_callee = true;
    }
  }
  record_check("leak_test: Caller -> Callee callee-saved registers scrubbed",
               !leak_to_callee, leak_to_callee ? 1 : 0, 0);

  // 試験 B1: 呼び先から呼び手への漏洩防止 (Callee -> Caller)
  // Caller 復帰直後、戻り値以外のスクラッチレジスタ (r2, r3, r12) に Callee のカナリア 0xCAFE.... が残っていないこと
  // caller_data[9] = r2, [10] = r3, [11] = r12
  bool leak_to_caller = (caller_data[9] == 0xCAFE0002u) ||
                        (caller_data[10] == 0xCAFE0003u) ||
                        (caller_data[11] == 0xCAFE000Cu);
  record_check("leak_test: Callee -> Caller scratch registers scrubbed",
               !leak_to_caller, leak_to_caller ? 1 : 0, 0);

  // 試験 B2: Caller 自身の callee-saved 復元 (Callee -> Caller)
  // Caller 復帰後、Caller 自身の callee-saved レジスタ (r4-r11) が元のカナリア 0xDEAD.... に復元されていること
  // caller_data[1..8] = r4..r11
  bool restored_caller = true;
  for (int i = 0; i < 8; ++i) {
    if (caller_data[1 + i] != (0xDEAD0004u + i)) {
      restored_caller = false;
    }
  }
  record_check("leak_test: Caller callee-saved registers restored",
               restored_caller, restored_caller ? 1 : 0, 1);
#endif
}

} // namespace shizuku::selftest
