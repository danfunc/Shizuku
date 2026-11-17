# Zephyr userspace IPC cost benchmark for RP2350 (Pico 2, Cortex-M33).
cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_ipc_bench)

# Custom syscalls (bench_stamp / bench_nop) are declared in include/bench_sc.h.
zephyr_syscall_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE src/main.c src/bench_sc.c)

# Optional variant: put the kernel and the ARM arch code in SRAM (-DBENCH_SRAM=ON,
# which build.sh passes with "sram"). Shizuku's RP2350 places its svc path in SRAM.
if(BENCH_SRAM)
  # Not the whole kernel library and not arm_mpu.c / arm_core_mpu.c: on RP2350 (XIP) the SRAM
  # is execute-never in the SoC's fixed MPU regions, and arch_kernel_init() (inlined in
  # z_cstart, kernel/init.c) enables the MPU in z_arm_mpu_init() *before*
  # z_arm_configure_static_mpu_regions() adds the executable region for the relocated text
  # (kernel_arch_func.h:49-56). Anything that runs in between, or returns into SRAM from the
  # MPU enable, takes an instruction-access fault. So init.c and the two MPU files stay in
  # flash (the MPU reprogramming on each switch therefore runs from flash), and only
  # steady-state kernel code that is first used after that is moved.
  set(KERNEL_DIR ${ZEPHYR_BASE}/kernel)
  foreach(f sched.c scheduler.c msg_q.c sem.c timeout.c sleep.c thread.c userspace/mem_domain.c)
    if(EXISTS ${KERNEL_DIR}/${f})
      zephyr_code_relocate(FILES ${KERNEL_DIR}/${f} LOCATION RAM)
    endif()
  endforeach()
  set(ARCH_ARM ${ZEPHYR_BASE}/arch/arm/core)
  zephyr_code_relocate(FILES ${ARCH_ARM}/userspace.S LOCATION RAM)
  zephyr_code_relocate(FILES ${ARCH_ARM}/cortex_m/svc.S LOCATION RAM)
  zephyr_code_relocate(FILES ${ARCH_ARM}/cortex_m/swap_helper.S LOCATION RAM)
  zephyr_code_relocate(FILES ${ARCH_ARM}/cortex_m/thread.c LOCATION RAM)
  zephyr_code_relocate(FILES ${ARCH_ARM}/cortex_m/exc_exit.c LOCATION RAM)
endif()
