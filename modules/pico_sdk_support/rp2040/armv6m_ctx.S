// Cortex-M0+ / ARMv6-M context switch. Only Thumb-1 instructions are used.
.syntax unified
.cpu cortex-m0plus
.thumb
.equ CTX_EXC_RETURN, 36
.equ CTX_CONTROL, 40
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
.macro SHIZUKU_TRACE_STAGE value
  // Preserve the live exception/C-call register set around the MMIO write.
  // Four words keep MSP alignment unchanged; r12, LR and r4-r11 are untouched.
  push {r0-r3}
  ldr r0, =0x4005801C
  ldr r1, =\value
  str r1, [r0]
  pop {r0-r3}
.endm
#endif
.macro CTX_SAVE
  // Keep MSP 8-byte aligned across the C call (AAPCS public interface).
  push {r4, lr}
  bl shizuku_current_context
  stmia r0!, {r4-r7}
  mov r1, r8
  mov r2, r9
  mov r3, r10
  mov r4, r11
  mrs r5, PSP
  stmia r0!, {r1-r5}
  subs r0, #36
  pop {r1, r4}
  str r4, [r0, #CTX_EXC_RETURN]
.endm
.macro CTX_RESTORE trace=0
  bl shizuku_arm_pending_step
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  .if \trace
  SHIZUKU_TRACE_STAGE 431
  .endif
#endif
  bl shizuku_restore_region_window
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  .if \trace
  SHIZUKU_TRACE_STAGE 432
  .endif
#endif
  bl shizuku_current_context
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  .if \trace
  // Preserve r0: it is the context pointer consumed by the following LDMIA.
  SHIZUKU_TRACE_STAGE 433
  .endif
#endif
  ldmia r0!, {r4-r7}
  ldmia r0!, {r1-r3}
  mov r8, r1
  mov r9, r2
  mov r10, r3
  ldmia r0!, {r1-r2}
  mov r11, r1
  msr PSP, r2
  ldr r3, [r0]
  ldr r1, [r0, #(CTX_CONTROL - CTX_EXC_RETURN)]
  mrs r2, CONTROL
  movs r0, #1
  ands r1, r0
  movs r0, #1
  bics r2, r0
  orrs r2, r1
  msr CONTROL, r2
  isb
  mov lr, r3
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  .if \trace
  SHIZUKU_TRACE_STAGE 434
  .endif
#endif
  bx lr
.endm
.global shizuku_armv6m_svc_entry
.type shizuku_armv6m_svc_entry,%function
shizuku_armv6m_svc_entry:
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  // Keep the marker independent of the C context lookup under investigation.
  SHIZUKU_TRACE_STAGE 400
#endif
  CTX_SAVE
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  SHIZUKU_TRACE_STAGE 401
#endif
  bl shizuku_svc_dispatch
#if defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  SHIZUKU_TRACE_STAGE 430
#endif
  CTX_RESTORE 1
.global shizuku_armv6m_pendsv_entry
.type shizuku_armv6m_pendsv_entry,%function
shizuku_armv6m_pendsv_entry:
  CTX_SAVE
  bl shizuku_pendsv_dispatch
  CTX_RESTORE
.global shizuku_armv6m_fault_entry
.type shizuku_armv6m_fault_entry,%function
shizuku_armv6m_fault_entry:
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  // Snapshot the hardware frame before CTX_SAVE calls into kernel state.
  // EXC_RETURN bit 2 selects the stack holding the frame; bit 3 identifies
  // whether the interrupted code was Thread or Handler mode.
  push {r4, lr}
  mov r1, lr
  movs r2, #4
  tst r1, r2
  beq 1f
  mrs r0, PSP
  b 2f
1:
  mrs r0, MSP
  adds r0, #8
2:
  bl shizuku_boot_hardfault_snapshot
3:
  b 3b
#else
  CTX_SAVE
  bl shizuku_fault_dispatch
  CTX_RESTORE
#endif
.global shizuku_armv6m_debugmon_entry
.type shizuku_armv6m_debugmon_entry,%function
shizuku_armv6m_debugmon_entry:
  CTX_SAVE
  bl shizuku_debug_dispatch
  CTX_RESTORE
.global shizuku_armv6m_systick_entry
.type shizuku_armv6m_systick_entry,%function
shizuku_armv6m_systick_entry:
  push {r4, lr}
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  mov r1, lr
  movs r2, #4
  tst r1, r2
  beq 1f
  mrs r0, PSP
  b 2f
1:
  mrs r0, MSP
  adds r0, #8
2:
  bl shizuku_armv6m_systick_dispatch_frame
#else
  bl shizuku_armv6m_systick_dispatch
#endif
  pop {r4, pc}
.global shizuku_armv6m_enter_thread_mode
.type shizuku_armv6m_enter_thread_mode,%function
shizuku_armv6m_enter_thread_mode:
  msr PSP, r0
  movs r0, #2
  msr CONTROL, r0
  isb
  bx r1
.global shizuku_armv6m_return_stub
.type shizuku_armv6m_return_stub,%function
shizuku_armv6m_return_stub:
  mov r2, r0
  movs r1, #1
  movs r3, #0
  mov r12, r3
  movs r0, #2
  svc 0
1: b 1b
