#ifndef SHIZUKU_CORE_NUM_HPP
#define SHIZUKU_CORE_NUM_HPP

#include <cstdint>

namespace shizuku {

// ARCH が core_num を持たない場合に native_call 等で型シグネチャを合わせるためのタグ
struct no_core final {};

template <class A>
concept arch_has_core_num = requires { typename A::core_num; };

template <class A, bool = arch_has_core_num<A>>
struct core_arg_helper {
  using type = no_core;
};

template <class A>
struct core_arg_helper<A, true> {
  using type = typename A::core_num;
};

template <class A>
using core_arg_t = typename core_arg_helper<A>::type;

// 前方宣言: ホスト単体テスト等で SIO を読まずに core_num を構築するためのアクセサ
template <class T>
struct core_num_test_access;

// ===========================================================================
//  コア番号型 (basic_core_num)
// ===========================================================================
//  マルチコア環境において生整数との混同を防ぎ、trap 入口等で Reader::read() により
//  ハードウェアから一度だけ取得したコア番号を、呼出し先へ型安全に渡し続けるための値型。
//  Reader ポリシーは static uint32_t read() noexcept を提供する。
//  構築時にのみ Reader::read() を呼び出し、以後 index() は保持する値を返す。
template <class Reader>
class basic_core_num final {
  uint32_t value_;

  struct ctor_from_value_tag {};
  basic_core_num(uint32_t v, ctor_from_value_tag) noexcept : value_(v) {}

  template <class> friend struct core_num_test_access;

public:
  explicit basic_core_num() noexcept : value_(Reader::read()) {}

  uint32_t index() const noexcept { return value_; }
};

} // namespace shizuku

#endif // SHIZUKU_CORE_NUM_HPP
