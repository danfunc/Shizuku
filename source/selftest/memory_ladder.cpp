// ===========================================================================
//  記憶の貸し借りが**定数時間**であることの実測 (DESIGN §16)
// ===========================================================================
//  ★「O(1) にした」は主張であって証拠ではない。証拠は「空きの散らばり方を変えても
//    費用が変わらない」という実測のほう。旧実装 (first-fit + 全走査の併合) は
//    空きが散らばるほど遅くなったので、そこに差が出るかを見る。
//
//  ★なぜ定数時間が要るか: svc ハンドラは定数時間で抜けるのに、その先の方針側が
//    「それまでの借り方」に依存して伸びるのでは最悪値が読めない。最悪値が読めない
//    と、実行権をどれだけ貸せばよいかの見積もりも立たない (貸し借りをクロック
//    基準にした D24 と同じ動機)。
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t SCATTER = 48;  // 散らかすために借りる本数
constexpr uintptr_t ROUNDS = 2000; // 1 回の計測で回す借り返しの回数
constexpr uintptr_t SMALL = 64;

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2);
  return {result.error, result.value};
}

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got,
                       want);
  }
}

// 借りて返すだけを ROUNDS 回。★毎回同じ大きさにするのは、測っているのが
// 「探す費用」であって「切り分ける費用」ではないことをはっきりさせるため。
uint64_t time_alloc_free() {
  const uint64_t started = BOARD::time_us();
  for (uintptr_t round = 0; round < ROUNDS; ++round) {
    const api_result got = api(object_api::MEMORY_ALLOCATE, SMALL);
    if (got.value == 0)
      return 0; // 借りられなくなったら測定不能
    api(object_api::MEMORY_RELEASE, got.value);
  }
  return BOARD::time_us() - started;
}

} // namespace

void memory_ladder() {
  BOARD::diag_printf("[SELFTEST] memory ladder start\n");

  // ---- 借りたものは自分のもので、返せる ------------------------------------
  const api_result mine = api(object_api::MEMORY_ALLOCATE, 256);
  check("memory: allocate", mine.error == (uintptr_t)object_error::OK &&
                                mine.value != 0,
        (unsigned long)mine.error, 0);
  const api_result owner = api(object_api::MEMORY_OWNER, mine.value);
  check("memory: the borrower is recorded",
        owner.value == api(object_api::GET_CURRENT_OBJECT).value,
        (unsigned long)owner.value,
        (unsigned long)api(object_api::GET_CURRENT_OBJECT).value);
  // ★取っ手が不正なら断ること。黙って何かを返すと、壊れた取っ手が系に広がる。
  const api_result bogus = api(object_api::MEMORY_RELEASE, mine.value + 8);
  check("memory: a bad handle is refused",
        bogus.error == (uintptr_t)object_error::BAD_MEMORY ||
            bogus.error == (uintptr_t)object_error::NOT_OWNER,
        (unsigned long)bogus.error,
        (unsigned long)object_error::BAD_MEMORY);
  check("memory: release", api(object_api::MEMORY_RELEASE, mine.value).error ==
                               (uintptr_t)object_error::OK,
        0, 0);

  // ---- 空きが散らばっても費用が変わらないこと ------------------------------
  // (1) きれいな状態 (空きは実質 1 つ) で測る。
  const uint64_t clean = time_alloc_free();

  // (2) 空きを散らかす: 借りて、1 つおきに返す。旧実装ならここから探索が伸びる。
  uintptr_t held[SCATTER];
  uintptr_t taken = 0;
  for (uintptr_t index = 0; index < SCATTER; ++index) {
    const api_result got = api(object_api::MEMORY_ALLOCATE, SMALL);
    if (got.value == 0)
      break;
    held[taken++] = got.value;
  }
  uintptr_t holes = 0;
  for (uintptr_t index = 0; index + 1 < taken; index += 2) {
    api(object_api::MEMORY_RELEASE, held[index]);
    held[index] = 0;
    ++holes;
  }
  const uint64_t scattered = time_alloc_free();

  // 後片付け (残りを返す)。
  for (uintptr_t index = 0; index < taken; ++index)
    if (held[index] != 0)
      api(object_api::MEMORY_RELEASE, held[index]);

  BOARD::diag_printf(
      "[SELFTEST] alloc+free x%lu: clean %luus, scattered over %lu holes "
      "%luus\n",
      (unsigned long)ROUNDS, (unsigned long)clean, (unsigned long)holes,
      (unsigned long)scattered);
  check("memory: holes were actually made", holes >= 8, (unsigned long)holes,
        8);
  // ★判定は「散らかしても 1.5 倍を超えない」。厳密な等値を求めないのは、
  //   割り込みや計測の粒度で数 % は動くため (等値を求める試験は嘘になる)。
  //   線形探索なら空きの数だけ伸びるので、この幅で十分に区別できる。
  check("memory: the cost does not follow the number of holes",
        clean != 0 && scattered <= clean + clean / 2,
        (unsigned long)scattered, (unsigned long)(clean + clean / 2));

  BOARD::diag_printf("[SELFTEST] memory ladder done\n");
}

} // namespace selftest
} // namespace shizuku
