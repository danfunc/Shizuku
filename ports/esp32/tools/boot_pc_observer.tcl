# Upper-layer diagnostic invocation only. init/target selection happen here.
# halt triggers the installed ESP target's WDT-disable event. This is not baseline.
proc checked {label script} {
    if {[catch {uplevel #0 $script} result]} {
        echo "$label failed: $result; target state unknown"
        shutdown error
        error "$label failed"
    }
}
checked init {init}
if {![info exists _TARGETNAME_0]} {
    echo "HP target name missing"
    shutdown error
    error "HP target name missing"
}
checked hp-target [list targets $_TARGETNAME_0]
checked sysbus [list $_TARGETNAME_0 riscv set_mem_access sysbus]
# These windows distinguish app vectors from esptool stub at the same addresses.
checked code-before-0 {echo [mdw 0x40800000 8]}
checked code-before-1 {echo [mdw 0x408003f0 8]}
for {set sample 0} {$sample < 3} {incr sample} {
    checked halt {halt 1000}
    echo "HALTED_SAMPLE=$sample (WDT state modified by target event)"
    checked registers {echo [get_reg -force {pc ra sp mepc mcause mtval mstatus mtvec}]}
    checked debug-reason {debug_reason}
    checked code-halted-0 {echo [mdw 0x40800000 8]}
    checked code-halted-1 {echo [mdw 0x408003f0 8]}
    checked resume {resume}
    if {$sample < 2} {sleep 200}
}
echo "DONE: target explicitly resumed; snapshots after first halt are perturbed"
shutdown
