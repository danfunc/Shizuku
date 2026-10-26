#!/usr/bin/env python3
"""Source ABI invariants; this does not claim execution on a C6."""
import re
from pathlib import Path
root = Path(__file__).absolute().parents[2]
layout = (root / 'arch/riscv/rv32_frame.h').read_text()
# Generic RV32 save/restore/CSR/mret/first-return/return stub (arch side).
asm = (root / 'arch/riscv/rv32_trap.S').read_text()
# C6-only 256B vector table with 32 slots that enter the generic entry (chip side).
vectors = (root / 'chips/esp32_c6/c6_vectors.S').read_text()
header = (root / 'arch/riscv/rv32.hpp').read_text()
offsets = dict((k, int(v)) for k, v in re.findall(r'#define RV32_F_(\w+) (\d+)', layout))
assert sorted(offsets.values()) == list(range(0, 144, 4))
assert '#define RV32_FRAME_BYTES 144' in layout
assert '#define RV32_MSTATUS_MPP 0x1800' in layout and '#define RV32_MSTATUS_MPIE 0x80' in layout
for name in offsets:
    reg = name.lower()
    assert f'offsetof(exception_frame_t, {reg}) == RV32_F_{name}' in header
    if reg in {'mepc', 'mstatus', 'mcause', 'mtval', 'reserved0', 'reserved1'}:
        continue
    assert f'sw {reg}, RV32_F_{name}(sp)' in asm
    assert f'lw {reg}, RV32_F_{name}(sp)' in asm
for csr in ('mepc', 'mstatus', 'mcause', 'mtval'):
    assert f'csrr t0, {csr}\n  sw t0, RV32_F_{csr.upper()}(sp)' in asm
assert 'csrw mepc, t0' in asm and 'csrw mstatus, t0' in asm and 'mret' in asm
# Vector table: 256B aligned, norvc, 32 slots, every slot enters the generic entry.
assert '.balign 256' in vectors and '.option norvc' in vectors and '.rept 32' in vectors
assert '  j shizuku_rv32_trap_entry\n.endr' in vectors
assert 'shizuku_rv32_trap_entry:' in asm and '.option norvc' in asm
assert 'csrrw t0, mscratch, zero\n  beqz t0, 1f\n  mv sp, t0' in asm
assert 'sw t0, 0(sp)' in asm and 'lw t0, 0(sp)\n' in asm
# 呼ぶ関数は t1 経由 (割込み・例外 = shizuku_rv32_trap_dispatch、ECALL = 番号の表の要素)。
assert 'la t1, shizuku_rv32_trap_dispatch\n.Lrv32_save_done:' in asm
assert asm.index('jalr t1') < asm.index('csrw mscratch, t0')
# ECALL: mepc を asm で進め (route_trap の ecall 枝に頼らない)、a0 の番号で表を引き、範囲外は 0 番。
ecall = asm.split('.Lrv32_full_save:')[0]
assert 'addi t0, t0, 4\n  sw t0, RV32_F_MEPC(sp)' in ecall
assert 'li t0, RV32_ECALL_TABLE_ENTRIES\n  bltu a0, t0, .Lrv32_ecall_in_range\n  li a0, 0\n' in ecall
assert 'slli a0, a0, 2\n  la t1, shizuku_rv32_ecall_table\n  add t1, t1, a0\n  lw t1, 0(t1)\n  j .Lrv32_save_done' in ecall
assert '#define RV32_ECALL_TABLE_ENTRIES 34' in layout
# 戻りの出口は 3 つ (カーネルはルータに徹し、レジスタは素通し。.handoff/v3-design-notes.md §5g):
#   RV32_TAG_ECALL       ECALL の普通の戻り。mepc / mstatus / a0 / a1 だけを読み、何も消さない。
#   RV32_TAG_ECALL_SCRUB callee が自分の意思で戻らない巻き戻し・別スレッドの再開。読むのは同じ 4 つで、
#                        ra・t*・a2-a7・s* を全部 0 に消す (gp / tp は caller stub が戻った直後に自分の
#                        値を入れ直すので触らない)。
#   RV32_TAG_ENTRY       呼び先の入口。ra と a0-a7 を読み、何も消さない。
# 再開フレーム (call_resume) は mepc / mstatus / 目印だけを書くので、消去はこの出口の仕事。
assert '#define RV32_TAG_ECALL 11' in layout and '#define RV32_TAG_ENTRY 0x100' in layout
assert '#define RV32_TAG_ECALL_SCRUB 0x101' in layout
assert '#define RV32_TAG_U_ENTRY 0x110' in layout
assert '#define RV32_TAG_U_ECALL 0x111' in layout
assert '#define RV32_TAG_U_SCRUB 0x112' in layout
restore = asm.split('shizuku_rv32_restore:')[1].split('.Lrv32_exit_ecall:')[0]
assert 'beq t0, t1, .Lrv32_exit_ecall' in restore and 'beq t0, t1, .Lrv32_exit_entry' in restore
assert 'li t1, RV32_TAG_ECALL_SCRUB\n  beq t0, t1, .Lrv32_exit_scrub' in restore
assert 'li t1, RV32_TAG_U_ENTRY\n  beq t0, t1, .Lrv32_umode_exit_entry' in restore
assert 'li t1, RV32_TAG_U_SCRUB\n  beq t0, t1, .Lrv32_umode_exit_scrub' in restore
assert 'li t1, RV32_TAG_U_ECALL\n  beq t0, t1, .Lrv32_umode_exit_ecall' in restore
exit_ecall = asm.split('.Lrv32_exit_ecall:')[1].split('// ---- ECALL からの戻り (RV32_TAG_ECALL_SCRUB')[0]
assert sorted(re.findall(r'lw (\w+), RV32_F_(\w+)\(sp\)', exit_ecall)) == sorted(
    [('t0', 'MEPC'), ('t0', 'MSTATUS'), ('a0', 'A0'), ('a1', 'A1')])
assert not re.findall(r'mv (\w+), zero', exit_ecall)  # 素通しの出口は何も消さない
assert exit_ecall.rstrip().endswith('mret')
exit_scrub = asm.split('.Lrv32_exit_scrub:')[1].split('// ---- 呼び先の入口')[0]
assert sorted(re.findall(r'lw (\w+), RV32_F_(\w+)\(sp\)', exit_scrub)) == sorted(
    [('t0', 'MEPC'), ('t0', 'MSTATUS'), ('a0', 'A0'), ('a1', 'A1')])
cleared = set(re.findall(r'mv (\w+), zero', exit_scrub))
expected_cleared = {'ra', *(f't{i}' for i in range(7)), *(f'a{i}' for i in range(2, 8)),
                    *(f's{i}' for i in range(12))}
assert cleared == expected_cleared, sorted(cleared ^ expected_cleared)
assert exit_scrub.rstrip().endswith('mret')
exit_entry = asm.split('.Lrv32_exit_entry:')[1].split('// ---- U-mode 高速出口')[0]
assert sorted(re.findall(r'lw (\w+), RV32_F_(\w+)\(sp\)', exit_entry)) == sorted(
    [('t0', 'MEPC'), ('t0', 'MSTATUS'), ('ra', 'RA')] + [(f'a{i}', f'A{i}') for i in range(8)])
assert not re.findall(r'mv (\w+), zero', exit_entry)  # 入口も何も消さない
assert exit_entry.rstrip().endswith('mret')
# U-mode 高速出口: 復帰先特権 U-mode 時に mscratch に trusted stack cursor、sp に U-mode sp を復帰
u_exit = asm.split('// ---- U-mode 高速出口 (RV32_TAG_U_*)')[1].split('.size shizuku_rv32_trap_entry')[0]
assert 'csrw mscratch, t0' in u_exit
assert 'lw sp, RV32_F_RESERVED0(sp)' in u_exit
# 再開フレームを作るのは call_resume_at (ヘッダ版の call_resume も、SHIZUKU_LEAN_LEDGER の台帳版もここへ来る)。
call_resume = header.split('static exception_frame_t *call_resume_at(')[1].split('\n  }\n')[0]
assert 'frame->mcause = scrub ? RV32_TAG_ECALL_SCRUB : RV32_TAG_ECALL' in call_resume
assert 'frame->mepc = pc' in call_resume
assert '*frame = {}' not in call_resume
# 別スレッドへ切り替わった再開は、ECALL で止まっていたフレームだけ消去の目印に替える。
scrub_on_switch = header.split('static void scrub_on_switch(')[1].split('\n  }\n')[0]
assert 'frame->mcause == RV32_TAG_ECALL' in scrub_on_switch
assert 'frame->mcause = RV32_TAG_ECALL_SCRUB' in scrub_on_switch
assert 'mv sp, a0\n  j shizuku_rv32_restore' in asm
assert 'shizuku_rv32_restore:' in asm and 'call shizuku_rv32_return_failed' in asm
assert 'shizuku_rv32_return_stub:' in asm and 'shizuku_rv32_first_return:' in asm
assert 'mv a2, a0' in asm and 'ecall' in asm
# Entry marker must not destroy ring.magic before retention validation.
diag = (root / 'modules/esp_idf_support/firmware/main/early_diag.c').read_text()
begin = diag.split('void shizuku_c6_early_diag_begin(void)')[1].split('void shizuku_c6_early_diag_writer_result')[0]
assert 'shizuku_c6_early_diag_log.magic = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC' not in begin
assert begin.index('shizuku_c6_early_diag_progress = SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC') < begin.index('esp_rom_get_reset_reason')

trap_src = (root / 'chips/esp32_c6/trap.cpp').read_text()
assert '__attribute__((weak))' not in trap_src
assert 'shizuku_esp_ble_' not in trap_src
assert 'shizuku_freertos_' not in trap_src
assert 'SHIZUKU_IRQ_DELAY_US' not in trap_src
assert 'shizuku_rv32_trap_dispatch' in trap_src and 'shizuku_rv32_return_failed' in trap_src
# 別のスレッドへ切り替わりうる出口 (ecall_run、ecall の svc_dispatch、周辺割込み、タイマー) は、
# 再開するスレッドの素通しの出口を消去の出口に替える (切り替わる前のスレッドの値を見せない)。
# 切替えが起きない ecall_direct には無い (上で context_to_frame を禁じている)。
assert trap_src.count('const auto *const entered = context;') == 4
assert trap_src.count('if (context != entered) arch::scrub_on_switch(*context);') == 4
# U-mode selftest assembly follows the ECALL caller ABI: returning raw ECALL probes preserve RA.
stage1 = (root / 'source/selftest/c6_stage1_safety.cpp').read_text()
for label, next_label in (
    ('.Lprobe_ecall', '.Lprobe_unpriv'),
    ('.Lprobe_unpriv', '.Lprobe_caller_stack'),
    ('.Lprobe_multi_trap_combo', '.Lprobe_u_to_u_call_1'),
):
    block = stage1.split(f'"{label}:\\n"', 1)[1].split(f'"{next_label}:\\n"', 1)[0]
    ecall_pos = block.index('"  ecall\\n"')
    assert block.index('"  sw ra, 12(sp)\\n"') < ecall_pos
    assert block.index('"  lw ra, 12(sp)\\n"', ecall_pos) > ecall_pos
    assert block.index('"  addi sp, sp, 16\\n"', ecall_pos) < block.rindex('"  ret\\n"')
c5_src = (root / 'chips/esp32_c5/trap.cpp').read_text()
assert c5_src.count('const auto *const entered = context;') == 3
assert c5_src.count('if (context != entered) arch::scrub_on_switch(*context);') == 3
board_src = (root / 'boards/esp32_c6/board_esp32_c6.cpp').read_text()
api_src = (root / 'chips/esp32_c6/trap_api.hpp').read_text()
assert 'void esp32_c6::init(uint32_t core)' in board_src
assert 'install_traps();' in board_src and 'interrupt_init();' in board_src
assert 'void esp32_c6::init' not in trap_src  # board 実装は chips に残さない
assert 'void install_traps();' in api_src and '[[noreturn]] void stop(uint32_t status);' in api_src
assert 'kernel.timer_expired()' in trap_src
assert 'run_pending_switch()' in trap_src
assert 'reg_write(c6::SYSTIMER_INT_CLR, c6::TARGET0_INT_BIT)' in trap_src
assert 'shizuku_c6_timer_diag' in trap_src
# ECALL は番号の表から直接入る。表は svc_attributes の handler 集合と static_assert で一致させ、
# ECALL の経路でも stack probe (1024 回に 1 回) を保つ。
assert 'shizuku_rv32_ecall_table = make_ecall_table()' in trap_src
assert 'ecall_table_consistent(' in trap_src
ecall_run = trap_src.split('inline arch::exception_frame_t *ecall_run(')[1].split('template <bool (shizuku::KERNEL::*Body)')[0]
assert 'check_stack_canary(0u)' in ecall_run and 'kStackProbeInterval' in ecall_run
# 実行権を渡す API (切替えが起きる) だけが記帳つきの ecall_run を通る (ecall_native)。
assert trap_src.count('ecall_native<&K::svc_') == 2
assert '&ecall_native<&K::svc_yield>' in trap_src and '&ecall_native<&K::svc_run_for>' in trap_src
# 切替えが起きない API は ecall_direct (1 API = 1 関数)。診断・入れ子の記帳・pend_flag を持たず、
# stack probe (1024 回に 1 回) と kernel 内からの ECALL (0xE003) の検査は残す。
ecall_direct = trap_src.split('inline arch::exception_frame_t *ecall_direct(')[1].split('using K = shizuku::KERNEL;')[0]
for forbidden in ('shizuku_c6_trap_diag', 'trap_depth =', 'active_frame', 'active_mstatus', 'pend_flag',
                  'current_context()', 'frame_to_context', 'context_to_frame'):
    assert forbidden not in ecall_direct, forbidden
assert 'kStackProbeInterval' in ecall_direct and 'trap_probe_interval_count' in ecall_direct
assert 'return ecall_other(frame);' in ecall_direct  # 1024 回目は記帳つきの経路で probe する
assert 'ecall_fault_nested(frame)' in ecall_direct and 'svc_native_route_of(thread)' in ecall_direct
assert 'stop(0xE003)' in trap_src.split('ecall_fault_nested(')[1].split('}')[0]
# 遅い経路はどれも tail call (成功経路に関数呼出しを持たない): 本体の中で呼ぶのは return 付きだけ。
import re as _re
assert not _re.search(r'(?<!return )\b(ecall_other|ecall_fault_\w+|ecall_to_kobj|stop)\(', ecall_direct)
for name, call in (('exit_method', 'exit_in_place<K, false>'), ('forward_child_exit', 'exit_in_place<K, true>'),
                   ('call_method', 'call_method_in_place'), ('call_descriptor', 'call_descriptor_in_place')):
    body = trap_src.split(f'ecall_{name}(arch::exception_frame_t *frame) {{')[1].split('\n}\n')[0]
    assert 'ecall_direct(frame' in body and call in body, name
for name in ('exit_method', 'call_method', 'get_current', 'get_caller', 'set_budget',
             'forward_child_exit', 'call_descriptor'):
    assert f'= &ecall_{name};' in trap_src, name

# ecall の caller 側 (rv32_abi.hpp): 既定は s1–s11 を全部 clobber (callee を信用しない)、gp/tp/s0 は asm で
# 退避・復元。試験的な遅延退避 (SHIZUKU_ABI_LAZY_SAVE、-fcall-used-s2..s11 と対で call_cost.cpp にだけ付く) は
# 別の inline 名前空間で、s1 を asm で退避して clobber から外し、s2–s11 は clobber に残す。
abi = (root / 'internal_headers/shizuku/rv32_abi.hpp').read_text()
assert abi.count(r'"sw gp, 32(sp)\n"') == 2 and abi.count(r'"sw tp, 36(sp)\n"') == 2
assert abi.count(r'"sw s0, 40(sp)\n" SHIZUKU_RV32_ABI_EXTRA_SAVE') == 2
lazy_block = abi.split('#if defined(SHIZUKU_ABI_LAZY_SAVE)\n#define SHIZUKU_RV32_ABI_EXTRA_SAVE')[1].split('#else')[0]
assert r'"sw s1, 44(sp)\n"' in lazy_block and r'"lw s1, 44(sp)\n"' in lazy_block
lazy_clobbers = lazy_block.split('SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS')[1]
assert '"s1"' not in lazy_clobbers and all(f'"s{i}"' in lazy_clobbers for i in range(2, 12))
default_block = abi.split('#else\n#define SHIZUKU_RV32_ABI_EXTRA_SAVE ""')[1].split('#endif')[0]
assert all(f'"s{i}"' in default_block for i in range(1, 12))  # 既定: s1–s11 全部 clobber
assert 'inline namespace lazy_save {' in abi and '} // inline namespace lazy_save' in abi
# callee 側: 使ったレジスタだけ消す属性は機能検出つきで、使えなければ空 (ビルドを壊さない)。
entry = (root / 'internal_headers/shizuku/method_entry.hpp').read_text()
assert '__has_attribute(zero_call_used_regs)' in entry and 'zero_call_used_regs("used-gpr")' in entry
assert 'defined(__riscv)' in entry and '#  define SHIZUKU_ZERO_USED_REGS\n' in entry

# 記述子の呼出し経路は「index → 世代照合 → 前計算の写しで入る」だけ。項目 (32 バイト) は呼び先の入口・親の入口・
# 種別・親・特権・窓・instance 世代と、照合用の持ち主と呼び先の世代を写しで持つ。表は [持ち主][index] の平坦配列で、
# 行は 128 バイトに揃えて id からシフトで引く。呼出しが行を引くのは世代照合の version の 1 語だけ。
ko = (root / 'internal_headers/shizuku/templates/kernel_object.hpp').read_text()
assert 'static constexpr uintptr_t OBJECT_ROW_ALIGN = 128;' in ko
assert 'struct alignas(OBJECT_ROW_ALIGN) object_t' in ko
assert 'sizeof(object_t) == OBJECT_ROW_ALIGN' in ko and 'sizeof(method_descriptor_entry) == 32' in ko
assert 'method_descriptor_entry m_descriptors[OBJECT_COUNT][METHOD_DESCRIPTOR_CAPACITY];' in ko
# 項目は前計算の写しを持つ (行への生ポインタは持たない)。
entry_struct = ko.split('struct method_descriptor_entry {')[1].split('};')[0]
for field in ('target_entry', 'parent_handler_entry', 'owner_generation', 'target_generation',
              'instance_generation', 'region_base', 'region_limit', 'callee', 'kind',
              'parent_handler_object', 'privileged', 'seq'):
    assert (f' {field};' in entry_struct or f' {field} :' in entry_struct or f' {field}:' in entry_struct), field
assert 'const object_t *target' not in entry_struct
assert 'static constexpr uint8_t DESCRIPTOR_NONE = 0xFF;' in ko
find_desc = ko.split('SHIZUKU_HOT const method_descriptor_entry *find_method_descriptor(')[1].split(
    'SHIZUKU_HOT callee_view view_of_descriptor')[0]
assert 'm_descriptors[caller_id][index]' in find_desc
assert find_desc.count('m_objects[') == 1 and 'm_objects[callee]' in find_desc  # 照合の version の 1 語だけ
assert 'desc.seq, __ATOMIC_ACQUIRE' in find_desc or '&desc.seq, __ATOMIC_ACQUIRE' in find_desc
view_desc = ko.split('SHIZUKU_HOT callee_view view_of_descriptor(')[1].split('// 呼び先の行から入場の形')[0]
assert 'm_objects[' not in view_desc and 'row' not in view_desc
pub = ko.split('void publish_descriptor(')[1].split('// 項目を無効にする')[0]
assert '__atomic_store_n(&desc.seq,' in pub  # 最後に偶数 seq で公開する
assert pub.rindex('__atomic_store_n') > pub.index('desc.callee')
nc = (root / 'internal_headers/shizuku/native_call.hpp').read_text()
call_desc = nc.split('inline void call_descriptor_in_place(')[1].split('// EXIT_METHOD (2, Forward')[0]
assert 'row' not in call_desc and 'kobj.row_id' not in call_desc and 'callee_row' not in call_desc
assert 'enter_in_place<K, KObj>(' in call_desc and 'kobj.view_of_descriptor(local_desc)' in call_desc
# trap 内の desc call 経路に表の錠 (table_lock) や LR/SC (cas32)、後方分岐の再試行ループが無いこと
assert 'table_lock' not in call_desc and 'table_unlock' not in call_desc
assert 'cas32' not in call_desc and 'lr.w' not in call_desc and 'sc.w' not in call_desc
assert 'while' not in call_desc and 'for ' not in call_desc and 'goto' not in call_desc
assert 'desc.seq' in call_desc
enter_body = nc.split('inline kernel_error enter_in_place(')[1].split('// CALL_METHOD (4)')[0]
assert 'row.' not in enter_body and 'kobj.' not in enter_body  # 入場は形 (view) だけを読む

# 行を書き換える操作のうち、写しの元になる値を変えるものは必ず呼び先の version を上げる (項目を STALE にする)。
# 上げるのは生成・差し替え・EXPORT_METHOD・GRANT_REGION の 4 つと init。SET_OBJECT_AFFINITY は版を上げない。
hd = (root / 'source/kernel_object/handler.cpp').read_text()
assert 'publish_descriptor(m_descriptors[caller_id][free_slot], target_id,' in hd
assert hd.count('invalidate_descriptor(d);') == 2  # init と差し替え
assert 'm_objects[target].generation, generation + 1u' in hd  # GRANT_REGION
assert 'm_objects[self].generation, generation + 1u' in hd  # EXPORT_METHOD
assert 'm_objects[id].generation,\n                             current_generation + 1u' in hd or \
       'current_generation + 1u' in hd  # 生成・差し替え
aff = hd.split('uintptr_t KERNEL_OBJECT::set_object_affinity(uintptr_t id, uintptr_t cores,')[1].split(
    'template <> SHIZUKU_HOT uint32_t KERNEL_OBJECT::object_region_base')[0]
assert 'generation' not in aff  # アフィニティの書き換えは版を上げない

boot_src = (root / 'modules/esp_idf_support/c6/early_boot.cpp').read_text()
assert 'shizuku_c6_thread1_diag' in boot_src
assert 'records[2]' in boot_src
assert 'thread1' in boot_src
assert 'c6_idf_ble_freertos_policy' in boot_src
assert 'set_irq_dispatch_hooks' in boot_src
assert 'set_irq_dispatch_hooks' in api_src
assert 'make_irq_dispatch_hooks' in api_src

# 計測の区間に入る caller/callee を C6 だけ .iram1 へ置く (SHIZUKU_HOT)。
# chip.hpp は rv32.hpp (経由で rv32_abi.hpp) より前に SHIZUKU_HOT を定義しないと stub が空に展開される。
chip_src = (root / 'chips/esp32_c6/chip.hpp').read_text()
assert chip_src.index('#define SHIZUKU_HOT ') < chip_src.index('#include "arch/riscv/rv32.hpp"')
assert 'section(".iram1." SHIZUKU_CHIP_STR(__COUNTER__))' in chip_src
abi_src = (root / 'internal_headers/shizuku/rv32_abi.hpp').read_text()
# テンプレートの入口 stub には付けない (__COUNTER__ が 1 回しか評価されず section type conflict になる)。
assert 'template <auto F> [[gnu::naked, gnu::used]] void method_stub_body()' in abi_src
assert 'SHIZUKU_HOT' not in abi_src.split('method_stub_body()')[0].rsplit('template <auto F>', 1)[-1]
trap_src = (root / 'chips/esp32_c6/trap.cpp').read_text()
assert 'SHIZUKU_HOT uint32_t plic_vector_mode::cycle_counter_read()' in trap_src
call_src = (root / 'source/selftest/call_cost.cpp').read_text()
for fn in ['cost_leaf', 'cost_nest', 'cost_dnest', 'cost_owner']:
    assert re.search(r'SHIZUKU_HOT SHIZUKU_ZERO_USED_REGS uintptr_t ' + fn + r'\(', call_src), fn
for fn in ['plain_callee', 'op_nothing', 'op_direct', 'op_svc', 'op_call', 'op_call2', 'op_desc1', 'op_desc2']:
    assert re.search(r'SHIZUKU_HOT __attribute__\(\(noinline\)\) uintptr_t ' + fn + r'\(', call_src), fn
assert 'SHIZUKU_HOT __attribute__((noinline)) stat_t measure(' in call_src
assert '#include "shizuku/hot.hpp"' in call_src

print('PASS: frame offsets, full saves/restores, CSR capture, vector slots, mscratch nesting, ring retention, timer/thread1 invariants')
