/* ISA / assembler probe only; this is not an exception handler or PMP setup.
 * Never call it from firmware. PMP entry layout and access policy are not
 * selected here; those require an explicit privilege-domain design. */
    .section .text
    .align  2
    .global shizuku_c6_arch_isa_probe
    .type   shizuku_c6_arch_isa_probe, @function
shizuku_c6_arch_isa_probe:
    csrr    a0, mstatus
    csrr    a1, pmpcfg0
    csrr    a2, pmpaddr0
    lr.w    t0, (a3)
    sc.w    a4, a4, (a3)
    ecall
    ret
    .size   shizuku_c6_arch_isa_probe, .-shizuku_c6_arch_isa_probe
