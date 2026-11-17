/*
 * Zephyr "service as thread + queue" IPC cost benchmark for RP2350 (Cortex-M33, 150 MHz).
 *
 * Mirrors bench/freertos_ipc/esp32c6/main/main.c: same conditions (1..6), same result
 * columns. Differences are Zephyr-specific:
 *   - callers and services are USER threads in separate k_mem_domain (USERSPACE),
 *   - the primary number is the MEAN of a loop of N=2048 round trips run by user
 *     threads that never read the clock; a privileged control thread (main) reads
 *     DWT CYCCNT before starting them and after the last one finishes, and an empty
 *     loop of the same shape (no call) is measured the same way and subtracted
 *     (same method as source/selftest/rp2350_u_cost.cpp). Rows named "*_m".
 *   - a distribution (min/p50/p90/p99/max) comes from a second pass in which the
 *     caller takes a timestamp with a custom syscall (bench_stamp) before and after
 *     each round trip; the syscall cost is measured by two back-to-back stamps and
 *     subtracted. Rows named "*_d".
 * Results are in RAM (zephyr_ipc_bench_results) and printed to the USB CDC console.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/app_memory/mem_domain.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/crc.h>
#include <stdarg.h>
#include <stdio.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/reboot.h>
#include <cmsis_core.h>
#include <pico/bootrom.h>
#include <boot/picoboot_constants.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bench_sc.h"

#define WARM 64u
#define NS 2048u
#define ROW (WARM + NS)
#define SVC_CONST 0x1234u
#define MAXC 8
#define MAXW 16
#define MAXD 64
#define PRIO_SVC_HI 2         /* services above the clients (K_PRIO_PREEMPT numbers: smaller = higher) */
#define PRIO_CLIENT 20
#define PRIO_BG 26
#define STK 1024
#define BG_STK 512
#define TMO_FINITE_MS 1000
#define BG_BEFORE 500         /* background wake = 500 + 3*i ticks  (< client timeout 1000) */
#define BG_AFTER 3000         /* background wake = 3000 + 3*i ticks (> client timeout 1000) */
#define SAFETY_MS 10000

/* ------------------------------------------------------------------ results */
#define NAME_LEN 32
#define MAX_RESULTS 96
#define FLAG_TIMEOUT 1u
#define FLAG_ABORTED 2u
#define FLAG_BADVAL 4u
#define FLAG_NOCLK 8u

struct bench_result {
	char name[NAME_LEN];
	uint32_t n, min, mean, max, p50, p90, p99;
	uint32_t param;
	uint32_t flags;
	uint32_t aux[4];
};

struct bench_results {
	uint32_t magic;       /* 0x5a504254 'ZPBT' */
	uint32_t version;
	uint32_t cpu_mhz;
	uint32_t tick_hz;
	uint32_t ovh_priv;    /* back-to-back bench_stamp() from a supervisor thread (cycles) */
	uint32_t count;
	uint32_t done;
	uint32_t ovh_user;    /* back-to-back bench_stamp() from a user thread (cycles) */
	uint32_t reserved[4];
	struct bench_result r[MAX_RESULTS];
};

struct bench_results zephyr_ipc_bench_results; /* RAM symbol read by the host script via CDC dump */
#define RES zephyr_ipc_bench_results

/* ------------------------------------------------------------------ memory domains */
K_APPMEM_PARTITION_DEFINE(part_caller);
K_APPMEM_PARTITION_DEFINE(part_svc);
K_APPMEM_PARTITION_DEFINE(part_shared);

static K_APP_BMEM(part_caller) uint32_t samp[MAXC * ROW];      /* caller-only */
static K_APP_BMEM(part_svc) volatile uint32_t svc_served;      /* service-only */
struct shared {
	volatile int32_t tmo_ms;      /* <0: forever */
	volatile uint32_t reply_q;    /* 1: reply by k_msgq, 0: by k_sem (+ reply_val) */
	volatile uint32_t n;
	volatile uint32_t err;
	volatile uint32_t done_n[MAXC];
	volatile uint32_t reply_val[MAXC];
};
static K_APP_BMEM(part_shared) struct shared sh;

static struct k_mem_domain dom_caller, dom_svc;

/* ------------------------------------------------------------------ kernel objects */
static struct k_msgq reqq;
static uint32_t reqq_buf[16];
static struct k_msgq respq[MAXC];
static uint32_t respq_buf[MAXC][4];
static struct k_sem respsem[MAXC];
static struct k_sem ctl_done;
static struct k_sem wk_sem;

static struct k_thread caller_t[MAXC], svc_t[MAXW], bg_t[MAXD], wk_t, cl_t;
K_THREAD_STACK_ARRAY_DEFINE(caller_ustk, MAXC, STK);
K_THREAD_STACK_ARRAY_DEFINE(svc_ustk, MAXW, STK);
K_KERNEL_STACK_ARRAY_DEFINE(caller_kstk, MAXC, STK);
K_KERNEL_STACK_ARRAY_DEFINE(svc_kstk, MAXW, STK);
K_KERNEL_STACK_ARRAY_DEFINE(bg_kstk, MAXD, BG_STK);
K_THREAD_STACK_DEFINE(wk_ustk, STK);
K_KERNEL_STACK_DEFINE(cl_kstk, STK);

static uint32_t seg[3][ROW];
static uint32_t tot[ROW];
static uint32_t ovh_priv, ovh_user;

static inline uint32_t cc(void) { return DWT->CYCCNT; }

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/* ------------------------------------------------------------------ output */
/* poll_out drops bytes once the CDC TX FIFO is full, so print only while a host has the
 * port open (DTR set). Rows are all re-dumped every 5 s after the run finishes. */
static bool host_attached(void)
{
	static const struct device *const cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0;
}

/*
 * Output. printk() -> uart_poll_out() drops bytes when the CDC TX buffer (one 64-byte USB
 * buffer until the class's tx work runs, usbd_cdc_acm.c:686-702 / :1141-1172: with flow
 * control off poll_out discards instead of waiting) is full, which cut every line after
 * ~64 bytes in the 3rd run. Now every line is written with uart_fifo_fill(), whose return
 * value says how much was accepted; the rest is retried after 1 ms (the tx work, which
 * runs in another thread, empties the buffer meanwhile), giving up on the line after 300 ms
 * without progress or when DTR drops. Each line ends with " *<crc32 hex>" over everything
 * before it so the host can discard damaged lines.
 */
static const struct device *const cdc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void tx_bytes(const uint8_t *p, int len)
{
	int64_t last = k_uptime_get();

	while (len > 0) {
		int n = uart_fifo_fill(cdc_dev, p, len);

		if (n > 0) {
			p += n;
			len -= n;
			last = k_uptime_get();
			continue;
		}
		if (k_uptime_get() - last > 300 || !host_attached()) {
			return;
		}
		k_msleep(1);
	}
}

static void out_line(const char *fmt, ...)
{
	char buf[384];
	va_list ap;

	if (!host_attached()) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 16, fmt, ap);

	va_end(ap);
	if (n < 0 || n > (int)sizeof(buf) - 16) {
		return;
	}
	n += snprintf(buf + n, sizeof(buf) - n, " *%08x\n", (unsigned int)crc32_ieee((uint8_t *)buf, n));
	tx_bytes((const uint8_t *)buf, n);
	uart_poll_out(cdc_dev, '\r'); /* kicks the tx work to flush a partly filled buffer */
	k_msleep(2);
}

/* ------------------------------------------------------------------ progress, faults, reboot */
#define CRASH_MAGIC 0xC4A5D00Du
#define MAX_FAULTS 4

struct fault_rec {
	uint32_t reason, pc, lr, xpsr, cfsr, mmfar, bfar, phase;
	char stage[NAME_LEN];
};
/* Faults of non-control threads are recorded here and the thread is aborted (the run goes on). */
static struct fault_rec faults[MAX_FAULTS];
static volatile uint32_t nfault;
/* Survives a warm reset if the chip keeps SRAM (not verified): written when the control
 * thread itself (or an ISR) faults, read back at the next boot. */
static __noinit struct fault_rec crash;
static __noinit uint32_t crash_magic;
static char stage_name[NAME_LEN];
static volatile uint32_t phase;   /* where inside session() we are, for fault records */
static k_tid_t ctl_tid;

static void stage(const char *name)
{
	strncpy(stage_name, name, NAME_LEN - 1);
	phase = 0;
}

/* Bail out of the remaining conditions once several threads have faulted. */
static bool aborted_run(void)
{
	return nfault >= 3;
}

/* One line per condition before it starts ("P <n> <name>"), so a stall shows where. */
static uint32_t progress_n;
static void progress(const char *name)
{
	stage(name);
	progress_n++;
	if (host_attached()) {
		out_line("P %u %s", progress_n, name);
	}
}

static void print_fault(const char *tag, const struct fault_rec *f)
{
	out_line("F %s reason=%u pc=0x%08x lr=0x%08x xpsr=0x%08x cfsr=0x%08x mmfar=0x%08x "
	       "bfar=0x%08x phase=%u stage=%s",
	       tag, f->reason, f->pc, f->lr, f->xpsr, f->cfsr, f->mmfar, f->bfar, f->phase,
	       f->stage);
}

/* Back to BOOTSEL. The bootrom lives at 0x0 and runs in privileged mode; Zephyr's MPU is
 * switched off first so no MPU region can stop the bootrom code (privileged default map
 * is enabled anyway, so this is belt and braces). rom_reboot() arms the RP2350 watchdog
 * (the bootrom does that) and the reset happens asynchronously, so this works with
 * interrupts masked and does not depend on the USB stack. reset_usb_boot() is the
 * fallback if rom_reboot() returns. */
static FUNC_NORETURN void to_bootsel(void)
{
	MPU->CTRL = 0;
	__DSB();
	__ISB();
	(void)rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 20, 0,
			 0);
	reset_usb_boot(0, 0);
	for (;;) {
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	struct fault_rec f;

	memset(&f, 0, sizeof(f));
	f.reason = reason;
	if (esf != NULL) {
		f.pc = esf->basic.pc;
		f.lr = esf->basic.lr;
		f.xpsr = esf->basic.xpsr;
	}
	f.cfsr = SCB->CFSR;
	f.mmfar = SCB->MMFAR;
	f.bfar = SCB->BFAR;
	f.phase = phase;
	memcpy(f.stage, stage_name, NAME_LEN - 1);
	if (!k_is_in_isr() && k_current_get() != ctl_tid && nfault < MAX_FAULTS) {
		faults[nfault] = f;
		nfault++;
		return; /* the faulting thread is aborted, the control thread carries on */
	}
	crash = f;
	crash_magic = CRASH_MAGIC;
	NVIC_SystemReset(); /* warm reset (SYSRESETREQ) */
	for (;;) {
	}
}

static void print_result(const struct bench_result *r)
{
	if (!host_attached()) {
		return;
	}
	out_line("R %s param=%u n=%u min=%u mean=%u max=%u p50=%u p90=%u p99=%u flags=%u "
	       "aux=%u,%u,%u,%u",
	       r->name, r->param, r->n, r->min, r->mean, r->max, r->p50, r->p90, r->p99,
	       r->flags, r->aux[0], r->aux[1], r->aux[2], r->aux[3]);
}

static void dump_all(void)
{
	if (!host_attached()) {
		return;
	}
	out_line("BEGIN magic=%u version=%u cpu=%uMHz tick=%uHz ovh_priv=%u ovh_user=%u count=%u done=%u",
	       RES.magic, RES.version, RES.cpu_mhz, RES.tick_hz, RES.ovh_priv, RES.ovh_user,
	       RES.count, RES.done);
	for (uint32_t i = 0; i < RES.count; i++) {
		print_result(&RES.r[i]);
	}
	for (uint32_t i = 0; i < nfault && i < MAX_FAULTS; i++) {
		print_fault("thread", &faults[i]);
	}
	out_line("END");
}

static struct bench_result *new_row(const char *name, uint32_t param, uint32_t flags)
{
	if (RES.count >= MAX_RESULTS) {
		return NULL;
	}
	struct bench_result *r = &RES.r[RES.count];

	memset(r, 0, sizeof(*r));
	strncpy(r->name, name, NAME_LEN - 1);
	r->param = param;
	r->flags = flags;
	RES.count++;
	return r;
}

/* Distribution row. `sub` is subtracted from each sample (stamp-syscall cost). */
static void finish_dist(const char *name, uint32_t param, uint32_t flags, const uint32_t *aux,
			uint32_t *s, uint32_t n, uint32_t sub)
{
	struct bench_result *r = new_row(name, param, flags);

	if (!r) {
		return;
	}
	if (aux) {
		for (int i = 0; i < 4; i++) {
			r->aux[i] = aux[i];
		}
	}
	r->n = n;
	if (n) {
		uint64_t sum = 0;

		for (uint32_t i = 0; i < n; i++) {
			s[i] = s[i] > sub ? s[i] - sub : 0;
			sum += s[i];
		}
		qsort(s, n, sizeof(uint32_t), cmp_u32);
		r->min = s[0];
		r->max = s[n - 1];
		r->mean = (uint32_t)(sum / n);
		r->p50 = s[(uint64_t)(n - 1) * 50 / 100];
		r->p90 = s[(uint64_t)(n - 1) * 90 / 100];
		r->p99 = s[(uint64_t)(n - 1) * 99 / 100];
	}
	print_result(r);
}

/* Loop-mean row: min/max/pXX are not available (0). aux = {T_raw, T_base, calls, mean*100}. */
static void finish_mean(const char *name, uint32_t param, uint32_t flags, uint32_t raw,
			uint32_t base, uint32_t calls)
{
	struct bench_result *r = new_row(name, param, flags);

	if (!r) {
		return;
	}
	uint32_t net = raw > base ? raw - base : 0;

	r->n = calls;
	r->mean = calls ? net / calls : 0;
	r->aux[0] = raw;
	r->aux[1] = base;
	r->aux[2] = calls;
	r->aux[3] = calls ? (uint32_t)(((uint64_t)net * 100u) / calls) : 0;
	print_result(r);
}

/* ------------------------------------------------------------------ thread bodies */
static void service_entry(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	const k_timeout_t tmo = sh.tmo_ms < 0 ? K_FOREVER : K_MSEC(sh.tmo_ms);
	const uint32_t rq = sh.reply_q;

	for (;;) {
		uint32_t v;

		if (k_msgq_get(&reqq, &v, tmo) != 0) {
			continue; /* timeout: wait again */
		}
		uint32_t idx = v >> 24;

		svc_served++;
		if (rq) {
			uint32_t k = SVC_CONST;

			k_msgq_put(&respq[idx], &k, K_NO_WAIT);
		} else {
			sh.reply_val[idx] = SVC_CONST;
			k_sem_give(&respsem[idx]);
		}
	}
}

/* Mean pass: no clock reads. */
static void caller_mean(void *a, void *b, void *c)
{
	(void)b; (void)c;
	const uint32_t idx = (uint32_t)(uintptr_t)a;
	const k_timeout_t tmo = sh.tmo_ms < 0 ? K_FOREVER : K_MSEC(sh.tmo_ms);
	const uint32_t rq = sh.reply_q, n = sh.n;
	uint32_t i = 0, r = 0;

	for (; i < n; i++) {
		uint32_t v = (idx << 24) | (i & 0xffffffu);

		if (k_msgq_put(&reqq, &v, tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
		if (rq) {
			if (k_msgq_get(&respq[idx], &r, tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
		} else {
			if (k_sem_take(&respsem[idx], tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
			r = sh.reply_val[idx];
		}
		if (r != SVC_CONST) { sh.err |= FLAG_BADVAL; }
	}
	sh.done_n[idx] = i;
	k_sem_give(&ctl_done);
}

/* Same loop shape with the call removed (the value check is kept). */
static void caller_empty(void *a, void *b, void *c)
{
	(void)b; (void)c;
	const uint32_t idx = (uint32_t)(uintptr_t)a;
	const uint32_t n = sh.n;
	volatile uint32_t sink;
	uint32_t i = 0, r = SVC_CONST;

	for (; i < n; i++) {
		uint32_t v = (idx << 24) | (i & 0xffffffu);

		sink = v;
		if (r != SVC_CONST) { sh.err |= FLAG_BADVAL; }
	}
	(void)sink;
	sh.done_n[idx] = i;
	k_sem_give(&ctl_done);
}

/* Distribution pass: one stamp syscall before and after each round trip. */
static void caller_dist(void *a, void *b, void *c)
{
	(void)b; (void)c;
	const uint32_t idx = (uint32_t)(uintptr_t)a;
	const k_timeout_t tmo = sh.tmo_ms < 0 ? K_FOREVER : K_MSEC(sh.tmo_ms);
	const uint32_t rq = sh.reply_q, n = sh.n;
	uint32_t *s = &samp[idx * ROW];
	uint32_t i = 0, r = 0;

	for (; i < n; i++) {
		uint32_t v = (idx << 24) | (i & 0xffffffu);
		uint32_t t0 = bench_stamp();

		if (k_msgq_put(&reqq, &v, tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
		if (rq) {
			if (k_msgq_get(&respq[idx], &r, tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
		} else {
			if (k_sem_take(&respsem[idx], tmo) != 0) { sh.err |= FLAG_TIMEOUT; break; }
			r = sh.reply_val[idx];
		}
		uint32_t t1 = bench_stamp();

		if (r != SVC_CONST) { sh.err |= FLAG_BADVAL; }
		s[i] = t1 - t0;
	}
	sh.done_n[idx] = i;
	k_sem_give(&ctl_done);
}

/* Syscall minimum. mode: 0 loop of bench_nop (mean), 1 stamp/nop/stamp (dist), 2 empty loop,
 * 3 stamp/stamp (calibration of the stamp syscall). */
static void nop_entry(void *a, void *b, void *c)
{
	(void)a; (void)c;
	const uint32_t mode = (uint32_t)(uintptr_t)b;
	const uint32_t n = sh.n;
	uint32_t *s = &samp[0];
	volatile uint32_t sink = 0;
	uint32_t i = 0;

	for (; i < n; i++) {
		if (mode == 0) {
			sink = bench_nop(i);
		} else if (mode == 1) {
			uint32_t t0 = bench_stamp();

			sink = bench_nop(i);
			s[i] = bench_stamp() - t0;
		} else if (mode == 2) {
			sink = i;
		} else {
			uint32_t t0 = bench_stamp();

			s[i] = bench_stamp() - t0;
		}
	}
	(void)sink;
	sh.done_n[0] = i;
	k_sem_give(&ctl_done);
}

static void bg_entry(void *a, void *b, void *c)
{
	(void)b; (void)c;
	const int32_t d = (int32_t)(uintptr_t)a;

	for (;;) {
		k_sleep(K_TICKS(d));
	}
}

/* ------------------------------------------------------------------ session helpers */
static void objs_init(uint32_t C)
{
	k_msgq_init(&reqq, (char *)reqq_buf, sizeof(uint32_t), 16);
	for (uint32_t i = 0; i < C; i++) {
		k_msgq_init(&respq[i], (char *)respq_buf[i], sizeof(uint32_t), 4);
		k_sem_init(&respsem[i], 0, 1);
	}
	k_sem_init(&ctl_done, 0, MAXC);
}

static void ctl_drain(void)
{
	while (k_sem_take(&ctl_done, K_NO_WAIT) == 0) {
	}
}

static void domains_init(void)
{
	k_mem_domain_init(&dom_caller, 0, NULL);
	k_mem_domain_add_partition(&dom_caller, &part_caller);
	k_mem_domain_add_partition(&dom_caller, &part_shared);
	k_mem_domain_add_partition(&dom_caller, &z_libc_partition); /* TLS pointer (z_arm_tls_ptr) */
	k_mem_domain_init(&dom_svc, 0, NULL);
	k_mem_domain_add_partition(&dom_svc, &part_svc);
	k_mem_domain_add_partition(&dom_svc, &part_shared);
	k_mem_domain_add_partition(&dom_svc, &z_libc_partition);
}

/*
 * One session: D background sleepers, W services, C callers; callers run `entry`.
 * user: services and callers are USER threads in dom_svc / dom_caller; otherwise
 * supervisor threads. Returns elapsed cycles between "start the callers" and
 * "all callers finished" (the ctl thread, which is above every other thread,
 * reads DWT; start/finish signalling cost is included). *flags gets error flags.
 */
static uint32_t session(int user, uint32_t C, uint32_t W, int svc_mode, int reply_q, int32_t tmo_ms,
			uint32_t D, uint32_t bg_base, k_thread_entry_t entry, uint32_t entry_arg_b,
			uint32_t *flags)
{
	sh.err = 0;
	sh.tmo_ms = tmo_ms;
	sh.reply_q = reply_q;
	sh.n = (entry == caller_dist) ? ROW : NS;
	if (entry == nop_entry) {
		sh.n = (entry_arg_b == 1 || entry_arg_b == 3) ? ROW : NS;
	}
	for (int i = 0; i < MAXC; i++) {
		sh.done_n[i] = 0;
	}
	ctl_drain();
	objs_init(C);
	phase = 1; /* creating background threads */
	for (uint32_t i = 0; i < D; i++) {
		k_thread_create(&bg_t[i], bg_kstk[i], K_KERNEL_STACK_SIZEOF(bg_kstk[i]), bg_entry,
				(void *)(uintptr_t)(bg_base + 3 * i), NULL, NULL, PRIO_BG, 0, K_NO_WAIT);
	}
	if (D) {
		k_sleep(K_MSEC(3)); /* let sleepers enter the timeout list */
	}
	for (uint32_t i = 0; i < W; i++) {
		int p = svc_mode == 0 ? PRIO_SVC_HI : svc_mode == 1 ? PRIO_SVC_HI + (int)i : PRIO_CLIENT;
		k_tid_t t;

		if (user) {
			t = k_thread_create(&svc_t[i], svc_ustk[i], K_THREAD_STACK_SIZEOF(svc_ustk[i]),
					    service_entry, NULL, NULL, NULL, p, K_USER, K_FOREVER);
			k_object_access_grant(&reqq, t);
			for (uint32_t k = 0; k < C; k++) {
				k_object_access_grant(&respq[k], t);
				k_object_access_grant(&respsem[k], t);
			}
			k_mem_domain_add_thread(&dom_svc, t);
		} else {
			t = k_thread_create(&svc_t[i], svc_kstk[i], K_KERNEL_STACK_SIZEOF(svc_kstk[i]),
					    service_entry, NULL, NULL, NULL, p, 0, K_FOREVER);
		}
		k_thread_start(t);
	}
	phase = 2; /* services started, waiting for them to block */
	k_sleep(K_MSEC(2)); /* services run once and block in k_msgq_get */
	phase = 3; /* creating callers */
	for (uint32_t i = 0; i < C; i++) {
		k_tid_t t;

		if (user) {
			t = k_thread_create(&caller_t[i], caller_ustk[i],
					    K_THREAD_STACK_SIZEOF(caller_ustk[i]), entry,
					    (void *)(uintptr_t)i, (void *)(uintptr_t)entry_arg_b, NULL,
					    PRIO_CLIENT, K_USER, K_FOREVER);
			k_object_access_grant(&reqq, t);
			k_object_access_grant(&respq[i], t);
			k_object_access_grant(&respsem[i], t);
			k_object_access_grant(&ctl_done, t);
			k_mem_domain_add_thread(&dom_caller, t);
		} else {
			t = k_thread_create(&caller_t[i], caller_kstk[i],
					    K_KERNEL_STACK_SIZEOF(caller_kstk[i]), entry,
					    (void *)(uintptr_t)i, (void *)(uintptr_t)entry_arg_b, NULL,
					    PRIO_CLIENT, 0, K_FOREVER);
		}
	}
	uint32_t f = 0;
	uint32_t t0 = cc();

	phase = 4; /* callers running */
	for (uint32_t i = 0; i < C; i++) {
		k_thread_start(&caller_t[i]);
	}
	for (uint32_t i = 0; i < C; i++) {
		/* after a thread fault, do not wait the full safety time for every condition */
		if (k_sem_take(&ctl_done, K_MSEC(nfault ? 1000 : SAFETY_MS)) != 0) {
			f |= FLAG_TIMEOUT | FLAG_ABORTED;
			break;
		}
	}
	uint32_t t1 = cc();

	phase = 5; /* teardown */
	for (uint32_t i = 0; i < C; i++) {
		if (f & FLAG_ABORTED) {
			k_thread_abort(&caller_t[i]);
		} else {
			k_thread_join(&caller_t[i], K_FOREVER);
		}
	}
	for (uint32_t i = 0; i < W; i++) {
		k_thread_abort(&svc_t[i]);
	}
	for (uint32_t i = 0; i < D; i++) {
		k_thread_abort(&bg_t[i]);
	}
	*flags = f | sh.err;
	return t1 - t0;
}

static void calibrate(void)
{
	uint32_t f, mx = 0;
	/* supervisor: back-to-back DWT-only syscall implementation */
	for (uint32_t i = 0; i < ROW; i++) {
		uint32_t t0 = bench_stamp();

		samp[i] = bench_stamp() - t0;
		if (samp[i] > mx) { mx = samp[i]; }
	}
	qsort(&samp[WARM], NS, sizeof(uint32_t), cmp_u32);
	ovh_priv = samp[WARM + NS / 2];
	RES.ovh_priv = ovh_priv;
	if (mx == 0) { RES.reserved[0] |= FLAG_NOCLK; }
	/* user: same, from a user thread */
	(void)session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 3, &f);
	qsort(&samp[WARM], NS, sizeof(uint32_t), cmp_u32);
	ovh_user = samp[WARM + NS / 2];
	RES.ovh_user = ovh_user;
}

/* Cached empty-loop totals per (user, C). */
static uint32_t base_cache[2][MAXC + 1];
static uint32_t base_total(int user, uint32_t C)
{
	if (!base_cache[user][C]) {
		uint32_t f;

		(void)session(user, C, 0, 0, 0, -1, 0, 0, caller_empty, 0, &f); /* warm */
		base_cache[user][C] = session(user, C, 0, 0, 0, -1, 0, 0, caller_empty, 0, &f);
	}
	return base_cache[user][C];
}

/* Conditions 1-4 rows. Writes "<name>_m" (loop mean) and "<name>_d" (distribution). */
static void run_rt(const char *name, int user, uint32_t C, uint32_t W, int svc_mode, int reply_q,
		   int32_t tmo_ms, uint32_t D, uint32_t bg_base, uint32_t param)
{
	char nm[NAME_LEN];
	uint32_t f = 0, f2 = 0;
	const uint32_t sub = user ? ovh_user : ovh_priv;

	if (aborted_run()) {
		return;
	}
	progress(name);
	uint32_t base = base_total(user, C);

	(void)session(user, C, W, svc_mode, reply_q, tmo_ms, D, bg_base, caller_mean, 0, &f2); /* warm */
	uint32_t raw = session(user, C, W, svc_mode, reply_q, tmo_ms, D, bg_base, caller_mean, 0, &f);

	snprintf(nm, sizeof(nm), "%s_m", name);
	finish_mean(nm, param, f | f2, raw, base, C * NS);

	(void)session(user, C, W, svc_mode, reply_q, tmo_ms, D, bg_base, caller_dist, 0, &f);
	/* compact post-warm samples of all callers */
	uint32_t total = 0;

	for (uint32_t c = 0; c < C; c++) {
		uint32_t k = sh.done_n[c] > WARM ? sh.done_n[c] - WARM : 0;

		memmove(&samp[total], &samp[c * ROW + WARM], k * sizeof(uint32_t));
		total += k;
	}
	uint32_t aux[4] = {D, W, C, (uint32_t)tmo_ms};

	snprintf(nm, sizeof(nm), "%s_d", name);
	finish_dist(nm, param, f, aux, samp, total, sub);
}

/* ------------------------------------------------------------------ condition 1: syscall minimum */
static void run_syscall_min(void)
{
	uint32_t f, f2;

	if (aborted_run()) {
		return;
	}
	progress("z1_syscall_min");
	(void)session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 0, &f2);
	uint32_t raw = session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 0, &f);
	(void)session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 2, &f2);
	uint32_t base = session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 2, &f2);

	finish_mean("z1_nop_user_m", 0, f, raw, base, NS);
	(void)session(1, 1, 0, 0, 0, -1, 0, 0, nop_entry, 1, &f);
	finish_dist("z1_nop_user_d", 0, f, NULL, &samp[WARM], NS, ovh_user);

	/* reference: the same call from a supervisor thread is a plain function call */
	for (uint32_t i = 0; i < ROW; i++) {
		uint32_t t0 = bench_stamp();

		(void)bench_nop(i);
		samp[i] = bench_stamp() - t0;
	}
	finish_dist("z1_nop_priv_d", 0, 0, NULL, &samp[WARM], NS, ovh_priv);
}

/* ------------------------------------------------------------------ condition 5: thread per request */
static void worker_entry(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	sh.reply_val[0] = SVC_CONST;
	k_sem_give(&wk_sem);
}

static void create_client(void *a, void *b, void *c)
{
	(void)b; (void)c;
	const int wprio = (int)(uintptr_t)a;
	uint32_t i = 0;

	for (; i < ROW; i++) {
		k_sem_init(&wk_sem, 0, 1);
		sh.reply_val[0] = 0;
		uint32_t t0 = cc();
		k_tid_t w = k_thread_create(&wk_t, wk_ustk, K_THREAD_STACK_SIZEOF(wk_ustk), worker_entry,
					    NULL, NULL, NULL, wprio, K_USER, K_FOREVER);

		k_object_access_grant(&wk_sem, w);
		k_mem_domain_add_thread(&dom_svc, w);
		k_thread_start(w);
		uint32_t t1 = cc();

		if (k_sem_take(&wk_sem, K_MSEC(TMO_FINITE_MS)) != 0) {
			sh.err |= FLAG_TIMEOUT;
			k_thread_abort(w);
			break;
		}
		uint32_t t2 = cc();

		k_thread_join(w, K_FOREVER);
		uint32_t t3 = cc();

		if (sh.reply_val[0] != SVC_CONST) { sh.err |= FLAG_BADVAL; }
		tot[i] = t3 - t0;
		seg[0][i] = t1 - t0;
		seg[1][i] = t2 - t1;
		seg[2][i] = t3 - t2;
	}
	sh.done_n[0] = i;
	k_sem_give(&ctl_done);
}

static void run_create(const char *tag, int wprio)
{
	if (aborted_run()) {
		return;
	}
	progress(tag);
	sh.err = 0;
	sh.done_n[0] = 0;
	ctl_drain();
	k_sem_init(&ctl_done, 0, MAXC);
	k_thread_create(&cl_t, cl_kstk, K_KERNEL_STACK_SIZEOF(cl_kstk), create_client,
			(void *)(uintptr_t)wprio, NULL, NULL, PRIO_CLIENT, 0, K_NO_WAIT);
	uint32_t flags = 0;

	if (k_sem_take(&ctl_done, K_MSEC(SAFETY_MS)) != 0) {
		flags = FLAG_TIMEOUT | FLAG_ABORTED;
		k_thread_abort(&cl_t);
		k_thread_abort(&wk_t);
	} else {
		k_thread_join(&cl_t, K_FOREVER);
	}
	flags |= sh.err;
	uint32_t k = sh.done_n[0] > WARM ? sh.done_n[0] - WARM : 0;
	static const char *const part[4] = {"total", "create", "wait", "join"};
	char nm[NAME_LEN];

	snprintf(nm, sizeof(nm), "z5_%s_%s", tag, part[0]);
	finish_dist(nm, wprio, flags, NULL, &tot[WARM], k, 0);
	for (int p = 1; p < 4; p++) {
		snprintf(nm, sizeof(nm), "z5_%s_%s", tag, part[p]);
		finish_dist(nm, wprio, flags, NULL, &seg[p - 1][WARM], k, 0);
	}
}

/* ------------------------------------------------------------------ condition 6: polling */
struct mailbox {
	volatile uint32_t req_seq, req_val, rsp_seq, rsp_val;
};
static struct mailbox mb;
static int poll_yield;
static uint32_t poll_total;
static volatile uint32_t poll_done;

static void poll_svc(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	uint32_t last = 0;

	for (;;) {
		while (mb.req_seq == last) {
			if (poll_yield) {
				k_yield();
			}
		}
		last = mb.req_seq;
		__DMB();
		(void)mb.req_val;
		mb.rsp_val = SVC_CONST;
		__DMB();
		mb.rsp_seq = last;
	}
}

static void poll_client(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	uint32_t i = 0;

	for (; i < poll_total; i++) {
		uint32_t t0 = cc();

		mb.req_val = i;
		__DMB();
		mb.req_seq = i + 1;
		while (mb.rsp_seq != i + 1) {
			if (poll_yield) {
				k_yield();
			}
		}
		__DMB();
		uint32_t r = mb.rsp_val;
		uint32_t t1 = cc();

		if (r != SVC_CONST) { sh.err |= FLAG_BADVAL; }
		tot[i] = t1 - t0;
		poll_done = i + 1;
	}
	k_sem_give(&ctl_done);
}

static void run_poll(const char *name, int yield, int svc_prio)
{
	if (aborted_run()) {
		return;
	}
	progress(name);
	sh.err = 0;
	poll_yield = yield;
	poll_total = ROW;
	poll_done = 0;
	memset(&mb, 0, sizeof(mb));
	ctl_drain();
	k_sem_init(&ctl_done, 0, MAXC);
	k_thread_create(&svc_t[0], svc_kstk[0], K_KERNEL_STACK_SIZEOF(svc_kstk[0]), poll_svc, NULL,
			NULL, NULL, svc_prio, 0, K_NO_WAIT);
	k_thread_create(&caller_t[0], caller_kstk[0], K_KERNEL_STACK_SIZEOF(caller_kstk[0]),
			poll_client, NULL, NULL, NULL, PRIO_CLIENT, 0, K_NO_WAIT);
	uint32_t flags = 0;

	if (k_sem_take(&ctl_done, K_MSEC(3000)) != 0) {
		flags = FLAG_TIMEOUT | FLAG_ABORTED;
	}
	uint32_t done = poll_done;

	k_thread_abort(&caller_t[0]);
	k_thread_abort(&svc_t[0]);
	flags |= sh.err;
	uint32_t k = done > WARM ? done - WARM : 0;
	uint32_t aux[4] = {done, WARM, ROW, (uint32_t)yield};

	finish_dist(name, svc_prio, flags, aux, &tot[WARM], k, 0);
}

/* ------------------------------------------------------------------ control */
int main(void)
{
	RES.magic = 0x5a504254u;
	RES.version = 1;
	RES.cpu_mhz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000;
	RES.tick_hz = CONFIG_SYS_CLOCK_TICKS_PER_SEC;

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	ctl_tid = k_current_get();

	/* Wait (at most 20 s) for a host to open the port, so it sees the progress lines. */
	for (int i = 0; i < 200 && !host_attached(); i++) {
		k_sleep(K_MSEC(100));
	}
	k_sleep(K_MSEC(300));
	if (crash_magic == CRASH_MAGIC) {
		/* The previous run ended in a fault of the control thread / an ISR. Report it
		 * (if the SRAM survived the warm reset) and go back to BOOTSEL; do not rerun. */
		crash_magic = 0;
		for (int i = 0; i < 40; i++) {
			if (host_attached()) {
				print_fault("previous-boot", &crash);
			}
			k_sleep(K_MSEC(500));
		}
		to_bootsel();
	}
	if (host_attached()) {
		out_line("S start cpu=%uMHz", RES.cpu_mhz);
	}
	progress("domains_init");
	domains_init();
	progress("calibrate");
	calibrate();
	if (host_attached())
	out_line("CAL ovh_priv=%u ovh_user=%u cpu=%uMHz", ovh_priv, ovh_user, RES.cpu_mhz);

	run_syscall_min();

	/* 2: privileged-to-privileged */
	run_rt("z2a_pp_hi_sem", 0, 1, 1, 0, 0, -1, 0, 0, 0);
	run_rt("z2b_pp_eq_sem", 0, 1, 1, 2, 0, -1, 0, 0, 0);
	run_rt("z2c_pp_hi_msgq", 0, 1, 1, 0, 1, -1, 0, 0, 0);

	/* 3: user-to-user, separate memory domains */
	run_rt("z3a_uu_hi_sem", 1, 1, 1, 0, 0, -1, 0, 0, 0);
	run_rt("z3b_uu_eq_sem", 1, 1, 1, 2, 0, -1, 0, 0, 0);
	run_rt("z3c_uu_hi_msgq", 1, 1, 1, 0, 1, -1, 0, 0, 0);

	/* 4: sweeps (user threads, k_sem reply) */
	static const uint32_t Ds[4] = {0, 4, 16, 64};
	char nm[NAME_LEN];

	for (int k = 0; k < 4; k++) {
		snprintf(nm, sizeof(nm), "z4d_tmo_bgbefore_D%u", Ds[k]);
		run_rt(nm, 1, 1, 1, 0, 0, TMO_FINITE_MS, Ds[k], BG_BEFORE, Ds[k]);
		snprintf(nm, sizeof(nm), "z4d_tmo_bgafter_D%u", Ds[k]);
		run_rt(nm, 1, 1, 1, 0, 0, TMO_FINITE_MS, Ds[k], BG_AFTER, Ds[k]);
		snprintf(nm, sizeof(nm), "z4d_forever_D%u", Ds[k]);
		run_rt(nm, 1, 1, 1, 0, 0, -1, Ds[k], BG_BEFORE, Ds[k]);
	}
	static const uint32_t Ws[3] = {1, 4, 16};

	for (int k = 0; k < 3; k++) {
		snprintf(nm, sizeof(nm), "z4w_prio_same_W%u", Ws[k]);
		run_rt(nm, 1, 1, Ws[k], 0, 0, -1, 0, 0, Ws[k]);
		snprintf(nm, sizeof(nm), "z4w_prio_diff_W%u", Ws[k]);
		run_rt(nm, 1, 1, Ws[k], 1, 0, -1, 0, 0, Ws[k]);
	}
	static const uint32_t Cs[4] = {1, 2, 4, 8};

	for (int k = 0; k < 4; k++) {
		snprintf(nm, sizeof(nm), "z4c_svc_hi_C%u", Cs[k]);
		run_rt(nm, 1, Cs[k], 1, 0, 0, -1, 0, 0, Cs[k]);
	}
	for (int k = 0; k < 4; k++) {
		snprintf(nm, sizeof(nm), "z4c_svc_eq_C%u", Cs[k]);
		run_rt(nm, 1, Cs[k], 1, 2, 0, -1, 0, 0, Cs[k]);
	}

	/* 5: thread per request */
	run_create("wk_hi", PRIO_SVC_HI);
	run_create("wk_eq", PRIO_CLIENT);

	/* 6: polling */
	run_poll("z6a_poll_eq_yield", 1, PRIO_CLIENT);
	run_poll("z6c_poll_hi_noyield", 0, PRIO_SVC_HI);

	RES.done = 1;
	stage("finished");
	/* Dump everything every 5 s. Back to BOOTSEL 30 s after the first dump that was sent
	 * with the host attached (DTR set before and after it), or 180 s after finishing if
	 * no host ever read a full dump. */
	int64_t t_done = k_uptime_get();
	int64_t t_read = -1;
	int ndump = 0; /* full dumps sent with the host attached */

	for (;;) {
		bool before = host_attached();

		dump_all();
		if (before && host_attached()) {
			ndump++;
			if (t_read < 0) {
				t_read = k_uptime_get();
			}
		}
		k_sleep(K_SECONDS(5));
		int64_t now = k_uptime_get();

		/* at least 5 dumps (the host keeps the damage-free copy of each row) and 30 s */
		if ((t_read >= 0 && ndump >= 5 && now - t_read >= 30000) || now - t_done >= 180000) {
			break;
		}
	}
	if (host_attached()) {
		out_line("REBOOT to BOOTSEL");
	}
	k_sleep(K_MSEC(300));
	to_bootsel();
}
