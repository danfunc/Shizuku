#!/bin/bash
# //firmware:esp32c6 の成果物 (アーカイブ / ELF / マップ / .bin) への assert。
#   使い方: test_image.sh <libshizuku_kernel.a> <app.elf> <app.map> <app.bin> <toolchain bin dir>
# ★archive pre-init entry のリンク・起動経路・USB marker・scheduler 参照不在を検査する。
#   実機出力や KERNEL 初期化の証明ではない。
set -u
ARCHIVE=$1
ELF=$2
MAP=$3
BIN=$4
TC=$5
NM=$TC/riscv32-esp-elf-nm
OBJDUMP=$TC/riscv32-esp-elf-objdump
AR=$TC/riscv32-esp-elf-ar
PYTHON3=$(command -v python3 || true)
# FAIL の記録先 (mktemp は環境によって使えないので、成果物の隣に置く)
FAILLOG=$BIN.failures.$$
: > "$FAILLOG"

# check <説明> <コマンド...>: コマンドが 0 で終われば PASS、そうでなければ FAIL を記録する。
check() (
  desc=$1
  shift
  if "$@" > /dev/null 2>&1; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc" >&2
    echo x >> "$FAILLOG"
  fi
)

members=$($AR t "$ARCHIVE" | wc -l | tr -d ' ')
syms_archive=$($NM -C --defined-only "$ARCHIVE" 2>/dev/null)
undef_archive=$($NM -C -u "$ARCHIVE" 2>/dev/null)
syms_elf=$($NM -C "$ELF")
raw_elf=$($NM "$ELF")
start_disasm=$($OBJDUMP -d --disassemble=start_cpu0 "$ELF")
entry_disasm=$($OBJDUMP -d --disassemble=shizuku_c6_early_boot "$ELF")
usb_disasm=$($OBJDUMP -d --disassemble=shizuku_c6_usb_console_write_and_flush "$ELF")

# --- A. アーカイブは空でなく、実カーネルのシンボルを含む
check "A1 archive has >= 6 members (got $members)" test "$members" -ge 6
check "A2 archive defines shizuku::kernel_instance" grep -q 'shizuku::kernel_instance' <<< "$syms_archive"
check "A3 archive defines KERNEL::init()" grep -q '::kernel<.*>::init()' <<< "$syms_archive"
check "A4 archive defines KERNEL::bootstrap(" grep -q '::kernel<.*>::bootstrap(' <<< "$syms_archive"
check "A5 archive defines KERNEL::do_switch(" grep -q '::kernel<.*>::do_switch(' <<< "$syms_archive"

# --- B. C6 構成は Pico (pico-sdk / hardware_*) に依存しない
check "B1 archive has no pico/hardware_ undefined refs" test -z "$(grep -i -E 'pico|hardware_|cyw43|btstack|tinyusb' <<< "$undef_archive")"
check "B2 ELF has no pico-sdk symbols" test -z "$(grep -i -E 'pico_|cyw43|btstack' <<< "$syms_elf")"
check "B3 map does not pull pico-sdk" test -z "$(grep -i 'pico-sdk' "$MAP")"

# --- C. カーネルの実コードが最終 ELF に残っている (GC されていない)
check "C1 ELF has kernel_instance (data)" grep -q 'shizuku::kernel_instance' <<< "$syms_elf"
check "C2 ELF has KERNEL::init() text" grep -q ' T .*::kernel<.*>::init()' <<< "$syms_elf"
check "C3 ELF has KERNEL::bootstrap( text" grep -q ' T .*::kernel<.*>::bootstrap(' <<< "$syms_elf"
check "C4 map and ELF identify every archive member occurrence" test -n "$PYTHON3"
if test -n "$PYTHON3"; then
  check "C4 member strong-symbol correspondence" "$PYTHON3" "$(dirname "$0")/check_archive_members.py" "$ARCHIVE" "$ELF" "$MAP" "$AR" "$NM"
fi

# --- D. 起動: start_cpu0 は archive pre-init entry を呼び、USB marker を保持する
start_addr=$(grep -E ' T start_cpu0$' <<< "$raw_elf" | awk '{print $1}')
check "D1 start_cpu0 comes from main/shizuku_boot.c.obj" grep -q 'shizuku_boot.c.obj' <<< "$(grep -E '^start_cpu0 ' "$MAP")"
diag_call_line=$(grep -n -m1 '<shizuku_c6_early_diag_begin>' <<< "$start_disasm" | cut -d: -f1)
entry_call_line=$(grep -n -m1 '<shizuku_c6_early_boot>' <<< "$start_disasm" | cut -d: -f1)
check "D2 start_cpu0 records boot before archive entry" test "$diag_call_line" -gt 0 -a "$entry_call_line" -gt "$diag_call_line"
check "D3 ELF retains early diagnostic capture function" grep -q ' T shizuku_c6_early_diag_begin$' <<< "$syms_elf"
check "D4 diagnostic capture comes from main/early_diag.c.obj" grep -q 'early_diag.c.obj' <<< "$(grep -E '^shizuku_c6_early_diag_begin ' "$MAP")"
check "D5 archive defines shizuku_c6_early_boot" grep -q ' T shizuku_c6_early_boot$' <<< "$syms_archive"
check "D6 ELF retains shizuku_c6_early_boot" grep -q ' T shizuku_c6_early_boot$' <<< "$syms_elf"
check "D7 early entry calls USB Serial/JTAG writer" grep -q '<shizuku_c6_usb_console_write_and_flush>' <<< "$entry_disasm"
check "D8 early entry records writer result after USB call" grep -q '<shizuku_c6_early_diag_writer_result>' <<< "$entry_disasm"
check "D9 ELF defines USB writer as text" grep -q ' T shizuku_c6_usb_console_write_and_flush$' <<< "$syms_elf"
check "D10 USB writer comes from main/usb_console.c.obj" grep -q 'usb_console.c.obj' <<< "$(grep -E '^shizuku_c6_usb_console_write_and_flush ' "$MAP")"
check "D11 USB route includes completion fence" grep -q 'fence' <<< "$usb_disasm"
check "D12 ELF retains noinit diagnostic log" grep -q ' B shizuku_c6_early_diag_log$' <<< "$syms_elf"
check "D13 diagnostic log comes from main/early_diag.c.obj" grep -q 'early_diag.c.obj' <<< "$(grep -E '^shizuku_c6_early_diag_log ' "$MAP")"
check "D14 ELF contains the boot marker" grep -aFq '[SHIZUKU] boot on esp32c6' "$ELF"
check "D15 early entry retains active nop idle" grep -qE '[[:space:]]nop([[:space:]]|$)' <<< "$entry_disasm"
check "D16 early entry has no WFI on either writer result" test -z "$(grep -E '[[:space:]]wfi([[:space:]]|$)' <<< "$entry_disasm")"
gaddr=$(grep -E ' R g_startup_fn$' <<< "$raw_elf" | awk '{print $1}')
word=$($OBJDUMP -s --start-address=$((16#$gaddr)) --stop-address=$((16#$gaddr + 4)) "$ELF" | tail -1 | awk '{print $2}')
le=$(echo "$word" | cut -c7-8)$(echo "$word" | cut -c5-6)$(echo "$word" | cut -c3-4)$(echo "$word" | cut -c1-2)
check "D17 g_startup_fn[0] ($le) == start_cpu0 ($start_addr)" test "$le" = "$start_addr"

# --- E. スケジューラ起動へ到達する経路が無い (ELF から物理的に消えている)
check "E1 vTaskStartScheduler absent from ELF" test -z "$(grep -E 'vTaskStartScheduler' <<< "$syms_elf")"
check "E2 esp_startup_start_app absent from ELF" test -z "$(grep -E 'esp_startup_start_app$' <<< "$syms_elf")"
check "E3 start_cpu0_default absent from ELF" test -z "$(grep -E 'start_cpu0_default' <<< "$syms_elf")"
check "E4 nothing in the ELF calls vTaskStartScheduler" test -z "$($OBJDUMP -d "$ELF" | grep -E '<vTaskStartScheduler>')"

# --- F. .bin は実体を持つ
check "F1 .bin is non-empty (> 64KiB)" test "$(wc -c < "$BIN" | tr -d ' ')" -gt 65536
check "F2 .bin starts with the ESP image magic 0xE9" test "$(head -c1 "$BIN" | xxd -p)" = "e9"

fails=$(grep -c x "$FAILLOG")
rm -f "$FAILLOG"
echo "fails=$fails"
test "$fails" -eq 0
