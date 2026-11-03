#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <type_traits>
#include "shizuku/templates/thread.hpp"

struct compact_context { static constexpr bool COMPACT_LEDGER = true; };
struct standard_context {};
using compact = shizuku::templates::thread<compact_context>::call_ledger_entry;
using standard = shizuku::templates::thread<standard_context>::call_ledger_entry;
static_assert(!std::is_same_v<compact, standard>);
static_assert(sizeof(uintptr_t) != 4 || sizeof(compact) == 20);

int main() {
  compact entry{72, 71, 0x12345678u, 2, 3, 33};
  entry.prev_call = 34;
  entry.ret_privileged = 1;
  entry.generation = UINT32_MAX;
  entry.ret_pc = 0x87654321u;
  entry.ret_sp = 0x76543210u;
  assert(entry.object == 72 && entry.handler_object == 71);
  assert(entry.kind == 2 && entry.via == 3 && entry.unwind_floor == 33);
  assert(entry.prev_call == 34 && entry.ret_privileged == 1);
  entry.ret_privileged = 3;
  assert(entry.ret_privileged > 1);
  assert(entry.handler_entry == 0x12345678u && entry.generation == UINT32_MAX);
  assert(entry.ret_pc == 0x87654321u && entry.ret_sp == 0x76543210u);

  // 欠陥 3 試験: (d) ledger の push->pop で caller_fpca が往復する (true/false 両方)
  for (bool want_fpca : {false, true}) {
    // 1. compact_call_ledger_entry 単体
    compact compact_entry{};
    compact_entry.ret_sp = 0x20001000u | (want_fpca ? 1u : 0u);
    assert(compact_entry.ret_fpca() == want_fpca);
    assert(compact_entry.caller_sp() == 0x20001000u);

    // 2. standard_call_ledger_entry 単体 (標準エントリは ret_fpca_val 専用、ret_sp はタグ無し)
    standard standard_entry{};
    standard_entry.ret_sp = 0x20001000u;
    standard_entry.ret_fpca_val = want_fpca ? 1 : 0;
    assert(standard_entry.ret_fpca() == want_fpca);
    assert(standard_entry.caller_sp() == 0x20001000u);

    // 3. compact ledger_accessor による記録 (push) と復元 (pop) の往復
    using compact_thread = shizuku::templates::thread<compact_context>;
    compact_thread thread{};
    compact thread_ledger[2]{};
    thread.ledger = thread_ledger;
    thread.ledger_capacity = 2;

    const bool recorded = compact_thread::ledger_accessor::record_caller_return_authority(
        thread, 0, 0x10002000u, 0x20001000u, 1, 0, 10, want_fpca);
    assert(recorded);
    assert(thread.ledger[0].ret_sp == (0x20001000u | (want_fpca ? 1u : 0u)));

    uintptr_t restored_pc = 0;
    uintptr_t restored_sp = 0;
    bool restored_priv = false;
    bool restored_fpca = !want_fpca;
    const bool restored = compact_thread::ledger_accessor::restore_caller_return_authority(
        thread, 0, restored_pc, restored_sp, restored_priv, &restored_fpca);
    assert(restored);
    assert(restored_pc == 0x10002000u);
    assert(restored_sp == 0x20001000u); // 必ず偶数 (マスク済み)
    assert((restored_sp & 1u) == 0);
    assert(restored_priv == true);
    assert(restored_fpca == want_fpca);

    // 4. standard ledger_accessor による記録 (push) と復元 (pop) の往復 (標準エントリは ret_sp タグ無し)
    using standard_thread = shizuku::templates::thread<standard_context>;
    standard_thread std_thread{};
    standard std_thread_ledger[2]{};
    std_thread.ledger = std_thread_ledger;
    std_thread.ledger_capacity = 2;

    const bool std_recorded = standard_thread::ledger_accessor::record_caller_return_authority(
        std_thread, 0, 0x10002000u, 0x20001000u, 1, 0, 10, want_fpca);
    assert(std_recorded);
    assert(std_thread.ledger[0].ret_sp == 0x20001000u); // 標準エントリはタグ無し
    assert(std_thread.ledger[0].ret_fpca_val == (want_fpca ? 1 : 0));

    uintptr_t std_restored_pc = 0;
    uintptr_t std_restored_sp = 0;
    bool std_restored_priv = false;
    bool std_restored_fpca = !want_fpca;
    const bool std_restored = standard_thread::ledger_accessor::restore_caller_return_authority(
        std_thread, 0, std_restored_pc, std_restored_sp, std_restored_priv, &std_restored_fpca);
    assert(std_restored);
    assert(std_restored_pc == 0x10002000u);
    assert(std_restored_sp == 0x20001000u);
    assert(std_restored_priv == true);
    assert(std_restored_fpca == want_fpca);
  }
}
