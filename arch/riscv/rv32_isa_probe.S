/*
 * Assembly syntax probe only. Do not link or call this in firmware.
 * In particular, ECALL has no installed trap handler in this probe.
 */
    .section .text
    .align  2
    .global shizuku_rv32_csr_probe
    .type   shizuku_rv32_csr_probe, @function
shizuku_rv32_csr_probe:
    csrr    a0, mstatus
    ret
    .size   shizuku_rv32_csr_probe, .-shizuku_rv32_csr_probe

    .global shizuku_rv32_atomic_probe
    .type   shizuku_rv32_atomic_probe, @function
shizuku_rv32_atomic_probe:
    lr.w    t0, (a0)
    sc.w    a1, a1, (a0)
    ret
    .size   shizuku_rv32_atomic_probe, .-shizuku_rv32_atomic_probe

    .global shizuku_rv32_ecall_probe
    .type   shizuku_rv32_ecall_probe, @function
shizuku_rv32_ecall_probe:
    ecall
    ret
    .size   shizuku_rv32_ecall_probe, .-shizuku_rv32_ecall_probe

