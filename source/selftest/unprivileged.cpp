// ===========================================================================
//  非特権オブジェクトが本当に非特権で走っているかを確かめる (DESIGN §11.2)
// ===========================================================================
//  ★「非特権で動いた」は**自己申告なしには言えない**。参照実装は、実際には特権の
//    まま走っていた計測を「非特権で動いた」と読んでしまい、そこから積み上げた観測が
//    全部無効になっている (§11.2.0)。だからここでは対象自身に CONTROL を読ませ、
//    その値を戻り値として持ち帰らせて突き合わせる。
//
//  ★プローブは**グローバルにも標準ライブラリにもカーネルの簿記にも触らない**。
//    それらは静的データ領域やペリフェラルにあり、region の外 = 特権のみなので、
//    触れば落ちる。落ちること自体は正しい動作だが、それを確かめるのは次の段
//    (拒否のテスト) で、まずは「非特権で走って戻ってこられる」ことだけを見る。
#include "shizuku/kernel.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {

uintptr_t unprivileged_control = 0;
uintptr_t privileged_control = 0;

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_UNPRIV_PROBE = object_id::unpriv_probe;
constexpr uintptr_t OBJECT_PRIV_PROBE = object_id::priv_probe;
constexpr uintptr_t OBJECT_TRESPASSER = object_id::trespasser;
constexpr uintptr_t METHOD_MAIN = 0;

// 自分の CONTROL を読んで返すだけ。触るのはレジスタと自分のスタックのみ。
uintptr_t probe(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return ARCH::control_register();
}

// ★拒否のテスト。許可のテストだけでは「保護が効いている」ことの証拠にならない
//   (DESIGN §16)。非特権オブジェクトに**触れないはずの場所**を触らせ、
//   (a) 落ちること (b) 落ちるのはこのスレッドだけで系は続くこと、を確かめる。
//   触る先は region の外にあり、かつ読んでも副作用の無い場所を選ぶ:
//   SIO の CPUID レジスタ (読み出し専用) — 万一通っても何も壊さない。
uintptr_t trespass(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const volatile uint32_t *forbidden = (const volatile uint32_t *)0xD0000000u;
  return *forbidden; // ここで落ちるのが正しい
}

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void unprivileged_probe() {
  BOARD::diag_printf("[SELFTEST] unprivileged probe start\n");

  // 同じコードを 2 つのオブジェクトとして登録する。**片方だけ非特権を宣言**する
  // ので、差が出れば「宣言が効いた」ことの証拠になる (対照実験)。
  ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_UNPRIV_PROBE,
                (uintptr_t)&probe, OBJECT_UNPRIVILEGED);
  ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_PRIV_PROBE,
                (uintptr_t)&probe, 0);

  const auto unprivileged =
      ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_UNPRIV_PROBE,
                    METHOD_MAIN, 0);
  const auto privileged = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                        OBJECT_PRIV_PROBE, METHOD_MAIN, 0);

  unprivileged_control = unprivileged.value;
  privileged_control = privileged.value;

  // CONTROL の bit0 (nPRIV) が 1 なら非特権。**対象が自分で読んだ値**である点が肝。
  check("unpriv: call returned", unprivileged.error == 0,
        (unsigned long)unprivileged.error, 0);
  check("unpriv: object reports non-privileged",
        (unprivileged.value & 1u) == 1u, (unsigned long)unprivileged.value, 1);
  check("unpriv: control object reports privileged",
        (privileged.value & 1u) == 0u, (unsigned long)privileged.value, 0);
  // 戻ってきた側 (呼び出し元) は特権のまま。非特権は呼び先の間だけで、
  // 呼び出しフレームの復元で自動的に戻る。
  check("unpriv: caller still privileged",
        (ARCH::control_register() & 1u) == 0u,
        (unsigned long)ARCH::control_register(), 0);

  // ---- 拒否のテスト: 触れないはずの場所を触ったら、そのスレッドだけが止まる ----
  {
    const auto faults_before = kernel_instance.faults().count;
    ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_TRESPASSER,
                  (uintptr_t)&trespass, OBJECT_UNPRIVILEGED);
    // ★**別スレッドで**走らせる。落ちるのはスレッド単位なので、呼び出しで試すと
    //   自分ごと止まってテストの続きが走れない。
    const auto spawned = ARCH::syscall((uintptr_t)object_api::SPAWN,
                                       OBJECT_TRESPASSER, METHOD_MAIN, 0);
    // 相手が走って落ちるまで譲る。ここで系が固まるなら「止め方」が壊れている。
    for (uint32_t guard = 0;
         guard < 200 && kernel_instance.faults().count == faults_before;
         ++guard)
      ARCH::syscall((uintptr_t)object_api::YIELD);

    const auto &record = kernel_instance.faults();
    check("deny: violation detected", record.count == faults_before + 1,
          (unsigned long)record.count, (unsigned long)(faults_before + 1));
    check("deny: the offending thread was stopped",
          record.thread == spawned.value, (unsigned long)record.thread,
          (unsigned long)spawned.value);
    check("deny: system still running", true, 1, 1);
    BOARD::diag_printf("[SELFTEST] deny: stopped thread %lu at pc=%08lx "
                       "cfsr=%08lx\n",
                       (unsigned long)record.thread, (unsigned long)record.pc,
                       (unsigned long)record.status);
  }

  BOARD::diag_printf("[SELFTEST] unprivileged probe done (unpriv control=%lu "
                     "priv control=%lu)\n",
                     (unsigned long)unprivileged.value,
                     (unsigned long)privileged.value);
}

} // namespace selftest
} // namespace shizuku
