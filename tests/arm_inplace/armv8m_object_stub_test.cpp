// オブジェクト専用の U 側 stub (ARM: SRAM だけで U を走らせる経路) の、カーネル側の配線を縛る試験。
//   - armv8m の stub 表: 登録のないオブジェクトは従来の flash の call_entry、登録したオブジェクトだけが
//     専用の call_entry (Thumb ビット付き) を引く。解除すると既定へ戻る。範囲外・不整列は拒否。
//   - set_entry_for_object が set_entry の「lr == return_stub()」枝と同じ形 (pc = call_entry,
//     lr = 本当の入口) を書く。
//   - native::set_callee_entry (enter_in_place / do_call が使う入口) が、専用 stub を持つ ARCH では
//     呼び先 id を渡して専用 stub 側へ、持たない ARCH (rv32 / armv6m / dummy) では従来の
//     set_entry(..., return_stub()) へ行く。
// stub image のコピー (copy_stub_image) は image 境界がリンカのラベルなので host では縛れない。
// 実機の SRAM-only 計測 ([COST-SRAM]) と ELF の逆アセンブルで確かめる。
#include <cassert>
#include <cstdint>
#ifdef NDEBUG
#error "This test requires assert()"
#endif
#include "arch/armv8m/armv8m.hpp"
#include "shizuku/kernel_abi.hpp"
#include "shizuku/native_call.hpp"
#include "shizuku/object_api.hpp"

// Link placeholders only: the ARM assembly stubs are not executed on host.
extern "C" void shizuku_armv8m_call_entry() {}
extern "C" void shizuku_armv8m_return_stub() {}
extern "C" const unsigned char shizuku_armv8m_stub_image_begin[4] = {};
extern "C" const unsigned char shizuku_armv8m_stub_image_end[4] = {};

using A = shizuku::archs::armv8m<8, 4>;

struct plain_arch {  // 専用 stub を持たない ARCH (set_entry だけ)
  struct frame_t { uint32_t pc = 0, lr = 0; };
  static uintptr_t return_stub() { return 0x1000; }
  static void set_entry(frame_t &f, uintptr_t pc, uintptr_t lr) { f.pc = (uint32_t)pc; f.lr = (uint32_t)lr; }
};
struct per_object_arch : plain_arch {  // 専用 stub を持つ ARCH
  static inline uint32_t seen_id = 0;
  static void set_entry_for_object(frame_t &f, uintptr_t pc, uint32_t id) {
    seen_id = id;
    f.pc = 0xAAAA0000u + id;
    f.lr = (uint32_t)pc;
  }
};

int main() {
  const uintptr_t flash_default = reinterpret_cast<uintptr_t>(&shizuku_armv8m_call_entry);
  constexpr uintptr_t entry = 0x20001235;
  alignas(32) unsigned char area[64]{};

  // 登録のないオブジェクト: 従来の flash の call_entry。lr に本当の入口。
  A::exception_frame_t f{};
  A::set_entry_for_object(f, entry, 5);
  assert(f.pc == static_cast<uint32_t>(flash_default));
  assert(f.lr == entry);
  assert(A::object_call_entry(5) == flash_default);

  // set_entry (lr == return_stub()) の枝と同じ結果になる。
  A::exception_frame_t g{};
  A::set_entry(g, entry, A::return_stub());
  assert(g.pc == f.pc && g.lr == f.lr);

  // 登録: 5 番だけが専用 call_entry (Thumb ビット付き)。ほかは既定のまま。
  assert(A::register_object_stubs(5, area));
  const uintptr_t special = A::object_call_entry(5);
  assert(special != flash_default && (special & 1u) == 1u);
  assert(A::object_call_entry(4) == flash_default && A::object_call_entry(6) == flash_default);
  A::exception_frame_t h{};
  A::set_entry_for_object(h, entry, 5);
  assert(h.pc == static_cast<uint32_t>(special) && h.lr == entry);
  A::exception_frame_t k{};
  A::set_entry_for_object(k, entry, 6);
  assert(k.pc == static_cast<uint32_t>(flash_default));

  // 拒否: 範囲外の id、null、4B 不整列。表は変わらない。
  assert(!A::register_object_stubs(static_cast<uint32_t>(A::kMaxObjectWindows), area));
  assert(!A::register_object_stubs(7, nullptr));
  assert(!A::register_object_stubs(7, area + 2));
  assert(A::object_call_entry(7) == flash_default);
  assert(!A::install_object_stubs(static_cast<uint32_t>(A::kMaxObjectWindows), area, sizeof(area)));

  // 解除で既定へ戻る。範囲外の解除は何もしない。
  A::uninstall_object_stubs(5);
  A::uninstall_object_stubs(static_cast<uint32_t>(A::kMaxObjectWindows));
  assert(A::object_call_entry(5) == flash_default);

  // enter_in_place / do_call が使う入口。専用 stub を持つ ARCH は呼び先 id を渡し、持たない ARCH は従来どおり。
  plain_arch::frame_t pf{};
  shizuku::native::set_callee_entry<plain_arch>(pf, entry, 9);
  assert(pf.pc == entry && pf.lr == plain_arch::return_stub());
  per_object_arch::frame_t qf{};
  shizuku::native::set_callee_entry<per_object_arch>(qf, entry, 9);
  assert(per_object_arch::seen_id == 9 && qf.pc == 0xAAAA0009u && qf.lr == entry);
  return 0;
}
