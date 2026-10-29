// modules/led_blink の試験。時刻と syscall は偽物 (fake_env) で置き換え、レジスタには触れない。
#include <cstdint>
#include <cstdio>
#include <vector>
#include "modules/led_blink/blink.hpp"
#include "modules/led_blink/blink_logic.hpp"

namespace lb = shizuku::led_blink;

#define CHECK(expr)                                                  \
  do {                                                               \
    if (!(expr)) {                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
      return 1;                                                      \
    }                                                                \
  } while (0)

namespace {

constexpr uint64_t PERIOD = 250000;
constexpr uintptr_t PARENT = 0;   // ROOT_OBJECT の模擬
constexpr uintptr_t RELAY = 1;    // 中継メソッドの番号の模擬
constexpr uintptr_t LED = 5;      // LED オブジェクトの ID の模擬

struct result_t {
  uintptr_t value;
  uintptr_t error;
};

struct call_t {
  uintptr_t number, a1, a2, a3;
  uint64_t at_us;
};

// 試験ごとに作り直す記録と、時刻・費用・遅れの操作つまみ。
struct fake_state {
  uint64_t now = 0;
  uint64_t extra_on_next_sleep = 0; // 次の SLEEP_US だけ余分に寝る (遅れの模擬)
  uint64_t write_cost = 0;          // CALL_METHOD 1 回の費用
  uintptr_t relay_error = 0;        // CALL_METHOD が返す error
  uint32_t writes_left = 0;         // running() の残り (0 で点滅が抜ける)
  std::vector<call_t> calls;
  std::vector<uint64_t> sleeps;
  std::vector<uint32_t> values;     // blink が書いた led_request.value
  std::vector<uint64_t> write_times;
  std::vector<uint64_t> lates;
};

fake_state g;

struct fake_env {
  static result_t syscall(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    g.calls.push_back({number, a1, a2, a3, g.now});
    if (number == (uintptr_t)shizuku::object_api::SLEEP_US) {
      g.sleeps.push_back(a1);
      g.now += a1 + g.extra_on_next_sleep;
      g.extra_on_next_sleep = 0;
      return {0, 0};
    }
    // CALL_METHOD: 中継 (親) でも LED の WRITE でも、書込み時刻と値を記録する。
    if (a3 != 0) {
      g.values.push_back(((const lb::led_request *)a3)->value);
      g.write_times.push_back(g.now);
    }
    g.now += g.write_cost;
    if (g.writes_left > 0 && a3 != 0)
      --g.writes_left;
    return {0, g.relay_error};
  }
  static uint64_t now_us() { return g.now; }
  static bool running() { return g.writes_left > 0; }
  static void on_period(uint64_t late_us) { g.lates.push_back(late_us); }
};

// 点滅スレッドを 1 本、呼び出し元 (スレッド) として走らせる。running() が false で戻る。
void run_blink() { lb::blink_entry<fake_env, PARENT, RELAY, PERIOD>(0, 0, 0, 0); }

void reset() { g = fake_state{}; }

// 試験 1: 時刻の計算だけ (周期前後、遅れ、反転、64bit 折り返し)。
int logic_test() {
  lb::schedule_t s{PERIOD, false};
  CHECK(lb::wait_us(s, 0) == PERIOD);
  CHECK(lb::wait_us(s, PERIOD - 1) == 1);
  CHECK(lb::wait_us(s, PERIOD) == 0);
  CHECK(lb::wait_us(s, PERIOD + 5) == 0);
  CHECK(lb::late_us(s, PERIOD - 1) == 0);
  CHECK(lb::late_us(s, PERIOD) == 0);
  CHECK(lb::late_us(s, PERIOD + 7) == 7);

  // ちょうど締切で反転し、締切は 1 周期後ろへ動く (前回の遅れは持ち越さない)。
  lb::advance(s, PERIOD);
  CHECK(s.on == true);
  CHECK(s.next_us == 2 * PERIOD);

  // 8 周期ぶん進めると毎回反転し、点灯は奇数回目で on。
  lb::schedule_t seq{PERIOD, false};
  for (uint64_t k = 1; k <= 8; ++k) {
    lb::advance(seq, PERIOD);
    CHECK(seq.on == (k % 2 == 1));
    CHECK(seq.next_us == (k + 1) * PERIOD);
  }

  // 締切が UINT64_MAX をまたぐ場合も差は符号付きで正しく測れる。
  lb::schedule_t wrap{UINT64_MAX - 10u, false};
  CHECK(lb::wait_us(wrap, UINT64_MAX - 20u) == 10u);
  CHECK(lb::wait_us(wrap, 5u) == 0u);
  CHECK(lb::late_us(wrap, 5u) == 16u); // 5 - (2^64 - 11) = 16 (mod 2^64)
  return 0;
}

// 試験 2: 点滅ループ本体。書込みは値の交互、締切は PERIOD の整数倍、寝る長さは残り時間。
int loop_test() {
  reset();
  g.writes_left = 6;
  run_blink();
  CHECK(g.values.size() == 6);
  for (size_t k = 0; k < g.values.size(); ++k) {
    CHECK(g.values[k] == (k % 2 == 0 ? 1u : 0u));
    CHECK(g.write_times[k] == (k + 1) * PERIOD);
  }
  CHECK(g.sleeps.size() == 6);
  for (uint64_t s : g.sleeps)
    CHECK(s == PERIOD);
  for (uint64_t l : g.lates)
    CHECK(l == 0);
  // 書込みは親の中継メソッドへ、親子呼出しで届く (番号と引数を確認)。
  for (const call_t &c : g.calls) {
    if (c.number == (uintptr_t)shizuku::object_api::CALL_METHOD) {
      CHECK(c.a1 == PARENT);
      CHECK(c.a2 == RELAY);
      CHECK(c.a3 != 0);
    }
  }
  return 0;
}

// 試験 3: 書込みに費用がかかっても締切は絶対グリッドのまま (費用が次周期へ積もらない)。
int drift_free_test() {
  reset();
  g.writes_left = 3;
  g.write_cost = 1000;
  run_blink();
  CHECK(g.write_times.size() == 3);
  CHECK(g.write_times[0] == PERIOD);
  CHECK(g.write_times[1] == 2 * PERIOD);
  CHECK(g.write_times[2] == 3 * PERIOD);
  return 0;
}

// 試験 4: 1 回だけ寝すぎたとき、その遅れは観測され、次の締切はグリッドへ戻る。
int late_recovery_test() {
  reset();
  g.writes_left = 2;
  g.extra_on_next_sleep = 300;
  run_blink();
  CHECK(g.lates.size() == 2);
  CHECK(g.lates[0] == 300);
  CHECK(g.lates[1] == 0);
  CHECK(g.write_times[0] == PERIOD + 300);
  CHECK(g.write_times[1] == 2 * PERIOD);
  return 0;
}

// 試験 5: 中継メソッドは LED の WRITE を親子呼出しで叩き、error をそのまま返す。
int relay_test() {
  reset();
  lb::led_request request{1};
  const uintptr_t error = lb::led_relay_entry<fake_env, LED>((uintptr_t)&request, 0, 0, 0);
  CHECK(error == 0);
  g.relay_error = 1002;
  const uintptr_t failed = lb::led_relay_entry<fake_env, LED>((uintptr_t)&request, 0, 0, 0);
  CHECK(failed == 1002);
  CHECK(!g.calls.empty());
  const call_t &last = g.calls.back();
  CHECK(last.number == (uintptr_t)shizuku::object_api::CALL_METHOD);
  CHECK(last.a1 == LED);
  CHECK(last.a2 == lb::LED_WRITE_METHOD);
  CHECK(last.a3 == (uintptr_t)&request);
  return 0;
}

} // namespace

int main() {
  if (logic_test() != 0) return 1;
  if (loop_test() != 0) return 1;
  if (drift_free_test() != 0) return 1;
  if (late_recovery_test() != 0) return 1;
  if (relay_test() != 0) return 1;
  std::puts("PASS: blink logic, loop alternation and grid, drift-free writes, late observation, relay");
  return 0;
}
