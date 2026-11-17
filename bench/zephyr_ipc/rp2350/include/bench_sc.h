/* Custom syscalls for the benchmark. */
#ifndef BENCH_SC_H_
#define BENCH_SC_H_
#include <zephyr/kernel.h>

/* Returns DWT->CYCCNT. Used to take one timestamp at a time from a user thread. */
__syscall uint32_t bench_stamp(void);

/* Does nothing; the verifier only checks the argument. Returns a constant. */
__syscall uint32_t bench_nop(uint32_t arg);

#include <zephyr/syscalls/bench_sc.h>
#endif
