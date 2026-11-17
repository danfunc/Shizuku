#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <cmsis_core.h>
#include "bench_sc.h"

uint32_t z_impl_bench_stamp(void)
{
	return DWT->CYCCNT;
}

static inline uint32_t z_vrfy_bench_stamp(void)
{
	return z_impl_bench_stamp();
}
#include <zephyr/syscalls/bench_stamp_mrsh.c>

uint32_t z_impl_bench_nop(uint32_t arg)
{
	(void)arg;
	return 0x1234u;
}

static inline uint32_t z_vrfy_bench_nop(uint32_t arg)
{
	/* "only checks the argument" */
	K_OOPS(K_SYSCALL_VERIFY(arg != 0xffffffffu));
	return z_impl_bench_nop(arg);
}
#include <zephyr/syscalls/bench_nop_mrsh.c>
