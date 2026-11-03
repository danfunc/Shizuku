#include "shizuku/core_num.hpp"
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

// (b) mutable_reader: 同じ Reader 型で値の変化を検証する
struct mutable_reader {
  static inline uint32_t value = 0;
  static inline int reads = 0;
  static uint32_t read() noexcept {
    ++reads;
    return value;
  }
};

using mutable_core = shizuku::basic_core_num<mutable_reader>;

uint32_t helper_const_ref(const mutable_core &core) {
  return core.index();
}

uint32_t helper_by_value(mutable_core core) {
  return core.index();
}

// (c) 独立した 2 つの Reader 型
struct fake_reader_c0 {
  static inline uint32_t read_count = 0;
  static uint32_t read() noexcept {
    read_count++;
    return 0;
  }
};

struct fake_reader_c1 {
  static inline uint32_t read_count = 0;
  static uint32_t read() noexcept {
    read_count++;
    return 1;
  }
};

using core0_t = shizuku::basic_core_num<fake_reader_c0>;
using core1_t = shizuku::basic_core_num<fake_reader_c1>;

// (d) copy-list-initialization (`core_num c = {};`) が不正であることを確認する concept
template <typename T>
void copy_list_init_sink(T);

template <typename T>
concept can_copy_list_init = requires {
  copy_list_init_sink<T>({});
};

} // namespace

int main() {
  // -------------------------------------------------------------------------
  // (b) mutable_reader: 同じ Reader 型で値の変化を検証
  //     reads=0, value=0 -> core a 構築(reads==1, a.index()==0)
  //     -> value=1 -> core b 構築(reads==2, b.index()==1)
  //     -> a,b を値コピー・const& 渡し・値渡しで複数 helper へ通し、
  //        さらに value を 0 に戻して helper を呼んでも reads==2 のまま、
  //        a.index()==0 と b.index()==1 が不変、コピーも同値。
  //        value 書換えで既存インスタンスの index() が変わらない(旧値 0 不変)。
  //        構築以外で Reader::read が呼ばれない(reads が増えない)。
  // -------------------------------------------------------------------------
  mutable_reader::reads = 0;
  mutable_reader::value = 0;

  mutable_core a;
  assert(mutable_reader::reads == 1);
  assert(a.index() == 0);

  mutable_reader::value = 1;
  mutable_core b;
  assert(mutable_reader::reads == 2);
  assert(b.index() == 1);

  // a, b を値コピー
  mutable_core a_copy = a;
  mutable_core b_copy = b;
  assert(a_copy.index() == 0);
  assert(b_copy.index() == 1);
  assert(mutable_reader::reads == 2);

  // const& 渡し・値渡しで複数 helper へ通す
  assert(helper_const_ref(a) == 0);
  assert(helper_by_value(a) == 0);
  assert(helper_const_ref(b) == 1);
  assert(helper_by_value(b) == 1);
  assert(helper_const_ref(a_copy) == 0);
  assert(helper_by_value(a_copy) == 0);
  assert(helper_const_ref(b_copy) == 1);
  assert(helper_by_value(b_copy) == 1);
  assert(mutable_reader::reads == 2);

  // さらに value を 0 に戻して helper を呼ぶ
  mutable_reader::value = 0;

  assert(helper_const_ref(a) == 0);
  assert(helper_by_value(a) == 0);
  assert(helper_const_ref(b) == 1);
  assert(helper_by_value(b) == 1);
  assert(helper_const_ref(a_copy) == 0);
  assert(helper_by_value(a_copy) == 0);
  assert(helper_const_ref(b_copy) == 1);
  assert(helper_by_value(b_copy) == 1);

  // reads==2 のまま、a.index()==0 と b.index()==1 が不変、コピーも同値
  assert(mutable_reader::reads == 2);
  assert(a.index() == 0);
  assert(b.index() == 1);
  assert(a_copy.index() == 0);
  assert(b_copy.index() == 1);

  // -------------------------------------------------------------------------
  // (c) 値 0 の reader で構築した core0 と 値 1 の reader で構築した core1 を並べて、
  //     別インスタンスが混線しない (一方の構築・コピーが他方へ影響しない)
  // -------------------------------------------------------------------------
  fake_reader_c0::read_count = 0;
  fake_reader_c1::read_count = 0;
  core0_t core0;
  core1_t core1;
  assert(core0.index() == 0);
  assert(core1.index() == 1);
  assert(fake_reader_c0::read_count == 1);
  assert(fake_reader_c1::read_count == 1);

  core0_t core0_copy = core0;
  assert(core0_copy.index() == 0);
  assert(core1.index() == 1);
  assert(fake_reader_c0::read_count == 1);
  assert(fake_reader_c1::read_count == 1);

  core1_t core1_copy = core1;
  assert(core0.index() == 0);
  assert(core1_copy.index() == 1);
  assert(fake_reader_c0::read_count == 1);
  assert(fake_reader_c1::read_count == 1);

  // -------------------------------------------------------------------------
  // (d) 静的検証: trivially copyable、暗黙変換不可、整数直接構築不可、explicit default 構築のみ
  // -------------------------------------------------------------------------
  static_assert(std::is_trivially_copyable_v<mutable_core>);
  static_assert(!std::is_convertible_v<mutable_core, uint32_t>);
  static_assert(!std::is_constructible_v<mutable_core, uint32_t>);
  static_assert(!can_copy_list_init<mutable_core>);

  static_assert(std::is_trivially_copyable_v<core0_t>);
  static_assert(!std::is_convertible_v<core0_t, uint32_t>);
  static_assert(!std::is_constructible_v<core0_t, uint32_t>);
  static_assert(!can_copy_list_init<core0_t>);

  static_assert(std::is_trivially_copyable_v<core1_t>);
  static_assert(!std::is_convertible_v<core1_t, uint32_t>);
  static_assert(!std::is_constructible_v<core1_t, uint32_t>);
  static_assert(!can_copy_list_init<core1_t>);

  std::printf("PASS: core_num_test (portable runtime)\n");
  return 0;
}
