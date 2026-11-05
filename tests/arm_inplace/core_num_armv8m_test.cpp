#include "shizuku/core_num.hpp"
#include "arch/armv8m/armv8m.hpp"
#include "core_num_test_access.hpp"
#include "arm_inplace_fixture.hpp"
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

// copy-list-initialization (`core_num c = {};`) が不正であることを確認する concept
template <typename T>
void copy_list_init_sink(T);

template <typename T>
concept can_copy_list_init = requires {
  copy_list_init_sink<T>({});
};

// ===========================================================================
// (1) 負のコンパイル検査:
// core 対応 ARCH に対し「core なし」呼出しが viable でないことの静的検証
// ===========================================================================
using adapter = test_inplace_adapter<shizuku::archs::armv8m<8, 4>>;
using kernel_t = mock_kernel<adapter>;

template <class K, class KObj>
concept can_call_method = requires(K &kernel, typename K::THREAD &thread,
                                   typename K::CONTEXT &ctx, KObj &kobj) {
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
};

template <class K, class KObj>
concept can_call_descriptor = requires(K &kernel, typename K::THREAD &thread,
                                       typename K::CONTEXT &ctx, KObj &kobj) {
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
};

template <class K, class KObj>
concept can_exit_in_place = requires(K &kernel, KObj &kobj, typename K::THREAD &thread,
                                     typename K::CONTEXT &ctx) {
  shizuku::native::exit_in_place<K, KObj, false>(kernel, kobj, thread, ctx);
};

template <class A>
concept can_defer_thread_switch = requires {
  shizuku::native::defer_thread_switch<A>();
};

template <class K, class KObj>
concept can_pop_in_place = requires(KObj &kobj, typename K::THREAD &thread,
                                    typename K::CONTEXT &ctx, typename K::FRAME *&frame) {
  shizuku::native::pop_in_place<K, KObj>(kobj, thread, ctx, frame, true, false);
};

template <class A, class Thread, class Context, class Frame>
concept can_header_pop_of = requires(Thread &thread, Context &ctx, Frame *&frame) {
  shizuku::header_pop_of<A>(thread, ctx, frame, true, true, false);
};

// core 対応 ARCH に対する「core なし」呼出しが viable でないこと
static_assert(!can_call_method<kernel_t, mock_kobj>,
              "call_method_in_place without core must not be viable for core-aware ARCH");
static_assert(!can_call_descriptor<kernel_t, mock_kobj>,
              "call_descriptor_in_place without core must not be viable for core-aware ARCH");
static_assert(!can_exit_in_place<kernel_t, mock_kobj>,
              "exit_in_place without core must not be viable for core-aware ARCH");
static_assert(!can_defer_thread_switch<adapter>,
              "defer_thread_switch without core must not be viable for core-aware ARCH");
static_assert(!can_pop_in_place<kernel_t, mock_kobj>,
              "pop_in_place without core must not be viable for core-aware ARCH");
static_assert(!can_header_pop_of<adapter, typename kernel_t::THREAD, typename kernel_t::CONTEXT, typename kernel_t::FRAME>,
              "header_pop_of without core must not be viable for core-aware ARCH");

// 正の検査: core 引数を渡せば viable
template <class K, class KObj, class Core>
concept can_call_method_with_core = requires(K &kernel, typename K::THREAD &thread,
                                             typename K::CONTEXT &ctx, KObj &kobj, const Core &core) {
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj, core);
};

template <class K, class KObj, class Core>
concept can_call_descriptor_with_core = requires(K &kernel, typename K::THREAD &thread,
                                                 typename K::CONTEXT &ctx, KObj &kobj, const Core &core) {
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj, core);
};

static_assert(can_call_method_with_core<kernel_t, mock_kobj, typename adapter::core_num>,
              "call_method_in_place with core must be viable for core-aware ARCH");
static_assert(can_call_descriptor_with_core<kernel_t, mock_kobj, typename adapter::core_num>,
              "call_descriptor_in_place with core must be viable for core-aware ARCH");

} // namespace

int main() {
  // -------------------------------------------------------------------------
  // (e) ARMv8-M ARCH の core_num が production Reader (sio_cpuid_reader) 型であること
  //     (sizeof <= 4, trivially copyable)。※実機の SIO は host で読まない (インスタンス構築しない)
  // -------------------------------------------------------------------------
  using armv8m = shizuku::archs::armv8m<8, 4>;
  static_assert(std::is_same_v<armv8m::core_num,
                               shizuku::basic_core_num<armv8m::sio_cpuid_reader>>);
  static_assert(sizeof(armv8m::core_num) <= 4);
  static_assert(std::is_trivially_copyable_v<armv8m::core_num>);
  static_assert(!std::is_convertible_v<armv8m::core_num, uint32_t>);
  static_assert(!std::is_constructible_v<armv8m::core_num, uint32_t>);
  static_assert(!can_copy_list_init<armv8m::core_num>);

  // -------------------------------------------------------------------------
  // (f) host test 用 factory (core_num_test_access): SIO を読まずに make(0/1) 構築
  // -------------------------------------------------------------------------
  const auto factory_c0 = shizuku::core_num_test_access<armv8m::core_num>::make(0);
  const auto factory_c1 = shizuku::core_num_test_access<armv8m::core_num>::make(1);
  assert(factory_c0.index() == 0);
  assert(factory_c1.index() == 1);
  assert(factory_c0.index() != factory_c1.index());
  const auto factory_c0_copy = factory_c0;
  assert(factory_c0_copy.index() == 0);
  assert(factory_c0_copy.index() != factory_c1.index());

  std::printf("PASS: core_num_armv8m_test\n");
  return 0;
}
