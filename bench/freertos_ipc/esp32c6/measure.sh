#!/bin/bash
# usage: measure.sh <dir containing shizuku_esp32c6_frtos_ipc.{bin,elf}>
# Flash with OpenOCD (same adapter as c6_measure.sh), wait for the benchmark
# to finish, dump the result struct over JTAG and print the table.
O=$(cd "$1" && pwd)
cd /Users/ishigakiyua/github/Shizuku/.handoff
HERE=/Users/ishigakiyua/github/Shizuku/.handoff/wt-frtos/bench/freertos_ipc/esp32c6
NM=/opt/homebrew/opt/llvm/bin/llvm-nm
SYM=frtos_ipc_bench_results
ADDR=$($NM $O/shizuku_esp32c6_frtos_ipc.elf | awk -v s=$SYM '$3==s{print "0x"$1}')
test -n "$ADDR" || { echo "symbol $SYM not found"; exit 1; }
SIZE=5424  # 48 + 64*84, see read_results.py
T=ipc-fastcall-c6-20261020/laneA-tools/openocd-esp32
OCD=($T/bin/openocd -s $T/share/openocd/scripts -f interface/esp_usb_jtag.cfg -f target/esp32c6.cfg -c "adapter serial 58:E6:C5:12:E3:98" -c "gdb port disabled" -c "tcl port disabled" -c "telnet port disabled")
"${OCD[@]}" -c "program_esp $O/shizuku_esp32c6_frtos_ipc.bin 0x10000 verify reset exit" > $O/flash-ocd.log 2>&1; echo "prog=$?"
for try in 1 2 3 4 5 6 7 8; do
  sleep 20
  "${OCD[@]}" -c init -c "targets esp32c6" -c "riscv set_mem_access sysbus" -c "dump_image $O/results.bin $ADDR $SIZE" -c shutdown > $O/sysbus.log 2>&1; echo "ocd=$? try=$try"
  python3 -I $HERE/read_results.py $O/results.bin > $O/results.txt; rc=$?
  [ $rc -ne 2 ] && break
done
cat $O/results.txt
exit $rc
