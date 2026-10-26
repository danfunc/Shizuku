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
#include "shizuku/method_entry.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/svc_attributes.hpp"

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
constexpr uintptr_t OBJECT_KERNEL_CALLER = object_id::kernel_caller;
constexpr uintptr_t OBJECT_WIN_A = object_id::win_a;
constexpr uintptr_t OBJECT_WIN_B = object_id::win_b;
constexpr uintptr_t METHOD_MAIN = 0;

// 非特権 probe に貸す data 窓。RP2040 (PMSAv6) は 2 の冪の大きさ・揃えでしか窓にできないので、
// 256B ちょうど・256B 境界に置く (RP2350 も同じ置き方で通る)。
alignas(256) uint8_t s_unpriv_probe_data[256] = {};

// 保護窓を持つ arch だけ、オブジェクトの data 窓を登録する。窓を持たない arch (RISC-V は chip 側)
// では何もしない。表現できない窓は登録されず false (= 非特権オブジェクトは data に触れない)。
// アプリ⇄ドライバの窓分離の試験用: 2 つの非特権オブジェクトが、それぞれ自分だけの data 窓を持つ。
alignas(256) uint8_t s_win_a_data[256] = {};
alignas(256) uint8_t s_win_b_data[256] = {};

// 自分の窓に書いて読み戻す。selector==1 は**相手の窓**を読む (窓の外なので落ちるのが正しい)。
uintptr_t win_a(uintptr_t selector, uintptr_t, uintptr_t, uintptr_t) {
  volatile uint8_t *own = s_win_a_data;
  own[0] = 0xA5;
  if (selector == 1) return *(volatile uint8_t *)s_win_b_data;
  return own[0];
}
uintptr_t win_b(uintptr_t selector, uintptr_t, uintptr_t, uintptr_t) {
  volatile uint8_t *own = s_win_b_data;
  own[0] = 0x5A;
  if (selector == 1) return *(volatile uint8_t *)s_win_a_data;
  return own[0];
}

// 窓の表を arch が持つか (ARM)。持たない arch (C6) は項目を足さず、既存の件数を変えない。
template <class A>
constexpr bool has_window_table = requires { A::set_object_bounds(0u, 0, 0, 0, 0); };

template <class A>
bool provision_data_window(uint32_t id, const void *base, uintptr_t bytes) {
  if constexpr (requires { A::set_object_bounds(id, 0, 0, 0, 0); }) {
    return A::set_object_bounds(id, (uintptr_t)base, (uintptr_t)base + bytes, 0, 0);
  } else {
    (void)id; (void)base; (void)bytes;
    return true;
  }
}

// 自分の CONTROL を読んで返すだけ。触るのはレジスタと自分のスタックのみ。
uintptr_t probe(uintptr_t selector, uintptr_t, uintptr_t, uintptr_t) {
  if (selector == 1)
    return ARCH::syscall((uintptr_t)object_api::GRANT_REGION, UINT32_MAX, 0, 0).error;
  if (selector == 2)
    return ARCH::syscall((uintptr_t)object_api::KILL_THREAD, UINT32_MAX).error;
  if (selector == 3)
    return ARCH::syscall((uintptr_t)object_api::FORWARD_CHILD_EXIT, 0, 0).error;
  if (selector == 4) // 自分自身への呼出し (許可)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_PRIV_PROBE, METHOD_MAIN, 0).error;
  if (selector == 5) // 兄弟への呼出し (NOT_PRIVILEGED)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_UNPRIV_PROBE, METHOD_MAIN, 0).error;
  if (selector == 6) // 親への呼出し (権限は許可、BAD_OBJECT/NOT_PRIVILEGED にならない)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, KERNEL_OBJECT::ROOT_OBJECT, 0, 0).error;
  if (selector == 7) // 存在しない相手への呼出し (BAD_OBJECT)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, KERNEL_OBJECT::OBJECT_COUNT, 0, 0).error;
  if (selector == 8) // 兄弟への BAD_METHOD 相当呼出し (NOT_PRIVILEGED が優先)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_UNPRIV_PROBE, KERNEL_OBJECT::METHOD_COUNT, 0).error;
  if (selector == 9) // 兄弟への UNDECLARED_METHOD 相当呼出し (NOT_PRIVILEGED が優先)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_UNPRIV_PROBE, 1, 0).error;
  if (selector == 10) // 許可される自分自身への BAD_METHOD 呼出し (BAD_METHOD が返る)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_PRIV_PROBE, KERNEL_OBJECT::METHOD_COUNT, 0).error;
  if (selector == 11) // 許可される自分自身への UNDECLARED_METHOD 呼出し (UNDECLARED_METHOD が返る)
    return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_PRIV_PROBE, 1, 0).error;
  return ARCH::control_register();
}

uintptr_t krole_probe(uintptr_t target, uintptr_t, uintptr_t, uintptr_t) {
  return ARCH::syscall((uintptr_t)object_api::CALL_METHOD, target, 0, 0).error;
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
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void unprivileged_probe() {
  BOARD::diag_printf("[SELFTEST] unprivileged probe start\n");

  // 同じコードを 2 つのオブジェクトとして登録する。**片方だけ非特権を宣言**する
  // ので、差が出れば「宣言が効いた」ことの証拠になる (対照実験)。
  if constexpr (has_window_table<ARCH>)
    check("window: probe data window registered",
          provision_data_window<ARCH>(OBJECT_UNPRIV_PROBE, s_unpriv_probe_data,
                                      sizeof(s_unpriv_probe_data)),
          1, 1);
  ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_UNPRIV_PROBE,
                method_entry<ARCH, &probe>(), OBJECT_UNPRIVILEGED);
  ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_PRIV_PROBE,
                method_entry<ARCH, &probe>(), 0);

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

  // Compare the actual handler decisions (not just its table) on both origins.
  for (uintptr_t selector = 1; selector <= 2; ++selector) {
    const uintptr_t id = selector == 1 ? (uintptr_t)object_api::GRANT_REGION
                                     : (uintptr_t)object_api::KILL_THREAD;
    const auto denied = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
        OBJECT_UNPRIV_PROBE, METHOD_MAIN, selector);
    const auto allowed = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
        OBJECT_PRIV_PROBE, METHOD_MAIN, selector);
    check(selector == 1 ? "attributes/handler GRANT deny matches" : "attributes/handler KILL deny matches",
        !svc_permission_allows(id, false) && denied.error == 0 &&
        denied.value == (uintptr_t)object_error::NOT_PRIVILEGED, denied.value,
        (uintptr_t)object_error::NOT_PRIVILEGED);
    check(selector == 1 ? "attributes/handler GRANT allow reaches target check" : "attributes/handler KILL allow reaches target check",
        svc_permission_allows(id, true) && allowed.error == 0 &&
        allowed.value == (uintptr_t)object_error::BAD_OBJECT, allowed.value,
        (uintptr_t)object_error::BAD_OBJECT);
  }
  for (uintptr_t object : {OBJECT_UNPRIV_PROBE, OBJECT_PRIV_PROBE}) {
    const auto denied = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
        object, METHOD_MAIN, 3);
    check("attributes/handler forwarding rejects missing parent ledger",
        !svc_permission_allows(27, object == OBJECT_PRIV_PROBE, false, false) &&
        denied.error == 0 && denied.value == (uintptr_t)object_error::NOT_PRIVILEGED,
        denied.value, (uintptr_t)object_error::NOT_PRIVILEGED);
  }

  // ---- CALL_METHOD 親子・権限チェックの検証 ----
  {
    // 1. 兄弟への CALL_METHOD は NOT_PRIVILEGED
    const auto sib_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                       OBJECT_PRIV_PROBE, METHOD_MAIN, 5);
    check("method parent check: sibling call refused with NOT_PRIVILEGED",
          sib_res.error == 0 && sib_res.value == (uintptr_t)object_error::NOT_PRIVILEGED,
          sib_res.value, (uintptr_t)object_error::NOT_PRIVILEGED);

    // 2. 自分自身への CALL_METHOD は OK
    const auto self_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                        OBJECT_PRIV_PROBE, METHOD_MAIN, 4);
    check("method parent check: self call allowed",
          self_res.error == 0 && self_res.value == (uintptr_t)object_error::OK,
          self_res.value, (uintptr_t)object_error::OK);

    // 3. 親 (ROOT_OBJECT) への CALL_METHOD は権限許可 (NOT_PRIVILEGED にならない)
    const auto parent_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                          OBJECT_PRIV_PROBE, METHOD_MAIN, 6);
    check("method parent check: parent call allowed by permission check",
          parent_res.error == 0 && parent_res.value != (uintptr_t)object_error::NOT_PRIVILEGED &&
              parent_res.value != (uintptr_t)object_error::BAD_OBJECT,
          parent_res.value, (uintptr_t)object_error::OK);

    // 4. 存在しない相手は BAD_OBJECT のまま
    const auto absent_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                          OBJECT_PRIV_PROBE, METHOD_MAIN, 7);
    check("method parent check: absent target returns BAD_OBJECT",
          absent_res.error == 0 && absent_res.value == (uintptr_t)object_error::BAD_OBJECT,
          absent_res.value, (uintptr_t)object_error::BAD_OBJECT);

    // 5. エラー順序: NOT_PRIVILEGED は BAD_METHOD / UNDECLARED_METHOD より先
    const auto sib_bm_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                          OBJECT_PRIV_PROBE, METHOD_MAIN, 8);
    check("method parent check: sibling BAD_METHOD yields NOT_PRIVILEGED",
          sib_bm_res.error == 0 && sib_bm_res.value == (uintptr_t)object_error::NOT_PRIVILEGED,
          sib_bm_res.value, (uintptr_t)object_error::NOT_PRIVILEGED);

    const auto sib_um_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                          OBJECT_PRIV_PROBE, METHOD_MAIN, 9);
    check("method parent check: sibling UNDECLARED yields NOT_PRIVILEGED",
          sib_um_res.error == 0 && sib_um_res.value == (uintptr_t)object_error::NOT_PRIVILEGED,
          sib_um_res.value, (uintptr_t)object_error::NOT_PRIVILEGED);

    // 許可される自分自身への BAD_METHOD / UNDECLARED_METHOD
    const auto self_bm_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                           OBJECT_PRIV_PROBE, METHOD_MAIN, 10);
    check("method parent check: self BAD_METHOD yields BAD_METHOD",
          self_bm_res.error == 0 && self_bm_res.value == (uintptr_t)object_error::BAD_METHOD,
          self_bm_res.value, (uintptr_t)object_error::BAD_METHOD);

    const auto self_um_res = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                           OBJECT_PRIV_PROBE, METHOD_MAIN, 11);
    check("method parent check: self UNDECLARED yields UNDECLARED_METHOD",
          self_um_res.error == 0 && self_um_res.value == (uintptr_t)object_error::UNDECLARED_METHOD,
          self_um_res.value, (uintptr_t)object_error::UNDECLARED_METHOD);

    // 6. kernel 役からの呼出しは相手を問わず許可
    // ★kernel_role 付与の正当性:
    //   「kernel 役 (kernel_role) を持つ呼び出し元は、親子関係にない相手 (兄弟など) であっても
    //   CALL_METHOD が即許可される」という仕様境界そのものを検証するための専用テストケースである。
    ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_KERNEL_CALLER,
                  method_entry<ARCH, &krole_probe>(),
                  KERNEL_OBJECT::INTERNAL_FLAG_KERNEL_ROLE);
    // kernel 役から兄弟 (OBJECT_UNPRIV_PROBE) を呼ぶ
    const auto krole_call = ARCH::syscall((uintptr_t)object_api::CALL_METHOD,
                                          OBJECT_KERNEL_CALLER, METHOD_MAIN,
                                          OBJECT_UNPRIV_PROBE);
    check("method parent check: kernel_role caller calls sibling successfully",
          krole_call.error == 0 && krole_call.value == (uintptr_t)object_error::OK,
          krole_call.value, (uintptr_t)object_error::OK);
  }

  // ---- 拒否のテスト: 触れないはずの場所を触ったら、そのスレッドだけが止まる ----
  {
    const auto faults_before = kernel_instance.faults().count;
    if constexpr (has_window_table<ARCH>)
      check("window: trespasser data window registered",
            provision_data_window<ARCH>(OBJECT_TRESPASSER, s_unpriv_probe_data,
                                        sizeof(s_unpriv_probe_data)),
            1, 1);
    ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_TRESPASSER,
                  method_entry<ARCH, &trespass>(), OBJECT_UNPRIVILEGED);
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

  // ---- 窓の分離 (窓の表を持つ arch だけ。C6 は chip 側の窓なので件数を変えない) ----
  if constexpr (has_window_table<ARCH>) {
    const bool provisioned =
        provision_data_window<ARCH>(OBJECT_WIN_A, s_win_a_data, sizeof(s_win_a_data)) &&
        provision_data_window<ARCH>(OBJECT_WIN_B, s_win_b_data, sizeof(s_win_b_data));
    check("window: two data windows registered", provisioned, 1, 1);
    ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_WIN_A,
                  method_entry<ARCH, &win_a>(), OBJECT_UNPRIVILEGED);
    ARCH::syscall((uintptr_t)object_api::CREATE_OBJECT, OBJECT_WIN_B,
                  method_entry<ARCH, &win_b>(), OBJECT_UNPRIVILEGED);
    // 呼ぶたびに窓が切り替わる: 自分の窓を触れて (切替え済み)、前に呼んだ側の窓が残っていれば落ちる。
    const auto a1 = ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_WIN_A, METHOD_MAIN, 0);
    const auto b1 = ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_WIN_B, METHOD_MAIN, 0);
    const auto a2 = ARCH::syscall((uintptr_t)object_api::CALL_METHOD, OBJECT_WIN_A, METHOD_MAIN, 0);
    check("window: A reaches its own window", a1.error == 0 && a1.value == 0xA5,
          (unsigned long)a1.value, 0xA5);
    check("window: B reaches its own window after A", b1.error == 0 && b1.value == 0x5A,
          (unsigned long)b1.value, 0x5A);
    check("window: A reaches its own window again", a2.error == 0 && a2.value == 0xA5,
          (unsigned long)a2.value, 0xA5);
    // 相手の窓を触ると、そのスレッドだけが止まる。
    const auto faults_before = kernel_instance.faults().count;
    const auto spawned = ARCH::syscall((uintptr_t)object_api::SPAWN, OBJECT_WIN_A, METHOD_MAIN, 1);
    for (uint32_t guard = 0;
         guard < 200 && kernel_instance.faults().count == faults_before; ++guard)
      ARCH::syscall((uintptr_t)object_api::YIELD);
    const auto &record = kernel_instance.faults();
    check("window: touching the other window faults", record.count == faults_before + 1,
          (unsigned long)record.count, (unsigned long)(faults_before + 1));
    check("window: only the offending thread stopped", record.thread == spawned.value,
          (unsigned long)record.thread, (unsigned long)spawned.value);
  }

  BOARD::diag_printf("[SELFTEST] unprivileged probe done (unpriv control=%lu "
                     "priv control=%lu)\n",
                     (unsigned long)unprivileged.value,
                     (unsigned long)privileged.value);
}

} // namespace selftest
} // namespace shizuku
