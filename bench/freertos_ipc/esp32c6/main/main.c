// FreeRTOS "service as task + queue" IPC cost benchmark for ESP32-C6.
// Plain ESP-IDF; all tasks run in M-mode, single core. Results are stored in
// the RAM struct `frtos_ipc_bench_results` (read over JTAG) and printed to the
// console one condition per line.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 0  // unknown; defaults file requests 160
#endif

#define WARM 64u
#define NS 2048u
#define ROW (WARM + NS)
#define SVC_CONST 0x1234u
#define MAXC 8
#define MAXW 16
#define MAXD 64
#define PRIO_CTL 23
#define PRIO_CLIENT 5
#define PRIO_SVC_HI 6
#define PRIO_BG 1
#define STK 2048
#define BG_STK 1280
#define SAFETY_TICKS 6000
#define TMO_FINITE 1000
#define BG_BEFORE 500   // background wake = 500 + 3*i ticks  (< client timeout 1000)
#define BG_AFTER 3000   // background wake = 3000 + 3*i ticks (> client timeout 1000)

_Static_assert(configMAX_PRIORITIES > PRIO_CTL, "need more priorities");

// ---------------------------------------------------------------- results
#define NAME_LEN 32
#define MAX_RESULTS 64
#define FLAG_TIMEOUT 1u  // a wait hit the safety timeout / queue op timed out
#define FLAG_ABORTED 2u  // condition was cut off by the controller
#define FLAG_BADVAL 4u   // a reply carried a wrong value
#define FLAG_NOCLK 8u    // cycle counter did not advance

typedef struct {
    char name[NAME_LEN];
    uint32_t n, min, mean, max, p50, p90, p99;
    uint32_t param;
    uint32_t flags;
    uint32_t aux[4];  // see aux_* in run_rt(); for others condition-specific
} bench_result_t;

typedef struct {
    uint32_t magic;  // 0x46524254 'FRBT'
    uint32_t version;
    uint32_t cpu_mhz;
    uint32_t tick_hz;
    uint32_t ovh;    // empty-loop cost (cycles) subtracted from every sample
    uint32_t count;  // number of valid entries in r[]
    uint32_t done;   // 1 when all conditions finished
    uint32_t reserved[5];
    bench_result_t r[MAX_RESULTS];
} frtos_bench_t;

frtos_bench_t frtos_ipc_bench_results;  // JTAG-visible symbol
#define RES frtos_ipc_bench_results

// ---------------------------------------------------------------- state
static uint32_t g_samp[MAXC * ROW];
static uint32_t g_seg[3][ROW];
static uint32_t g_done_n[MAXC];
static volatile uint32_t g_err;
static uint32_t g_ovh;

static TaskHandle_t g_ctl;
static TaskHandle_t g_client_h[MAXC];
static TaskHandle_t g_svc_h[MAXW];
static TaskHandle_t g_bg_h[MAXD];
static QueueHandle_t g_reqq, g_respq[MAXC];
static StaticQueue_t g_reqq_cb, g_respq_cb[MAXC];
static uint32_t g_reqq_buf[16], g_respq_buf[MAXC][4];
static TickType_t g_tmo;
static int g_reply_queue;

static StaticTask_t g_cl_tcb[MAXC], g_svc_tcb[MAXW], g_bg_tcb[MAXD], g_wk_tcb;
static StackType_t g_cl_stk[MAXC][STK] __attribute__((aligned(16)));
static StackType_t g_svc_stk[MAXW][STK] __attribute__((aligned(16)));
static StackType_t g_bg_stk[MAXD][BG_STK] __attribute__((aligned(16)));
static StackType_t g_wk_stk[STK] __attribute__((aligned(16)));

static inline uint32_t cc(void) { return (uint32_t)esp_cpu_get_cycle_count(); }

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void print_result(const bench_result_t *r) {
    printf("R %s param=%u n=%u min=%u mean=%u max=%u p50=%u p90=%u p99=%u flags=%u "
           "aux=%u,%u,%u,%u\n",
           r->name, (unsigned)r->param, (unsigned)r->n, (unsigned)r->min,
           (unsigned)r->mean, (unsigned)r->max, (unsigned)r->p50, (unsigned)r->p90,
           (unsigned)r->p99, (unsigned)r->flags, (unsigned)r->aux[0],
           (unsigned)r->aux[1], (unsigned)r->aux[2], (unsigned)r->aux[3]);
}

// Subtract the empty-loop cost, sort, and record. `sub` is how many times the
// cycle-counter read cost is included in a sample (1 for a start/end pair).
static void finish(const char *name, uint32_t param, uint32_t flags, const uint32_t *aux,
                   uint32_t *s, uint32_t n) {
    if (RES.count >= MAX_RESULTS) return;
    bench_result_t *r = &RES.r[RES.count];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, NAME_LEN - 1);
    r->param = param;
    r->flags = flags;
    if (aux)
        for (int i = 0; i < 4; i++) r->aux[i] = aux[i];
    r->n = n;
    if (n) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            s[i] = s[i] > g_ovh ? s[i] - g_ovh : 0;
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
    RES.count++;
    print_result(r);
}

static void calibrate(void) {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < ROW; i++) {
        uint32_t t0 = cc();
        uint32_t t1 = cc();
        g_samp[i] = t1 - t0;
        if (g_samp[i] > mx) mx = g_samp[i];
    }
    uint32_t *s = &g_samp[WARM];
    qsort(s, NS, sizeof(uint32_t), cmp_u32);
    g_ovh = s[NS / 2];
    RES.ovh = g_ovh;
    if (mx == 0) RES.reserved[0] |= FLAG_NOCLK;
}

// ---------------------------------------------------------------- tasks
static void reply(uint32_t v) {
    uint32_t idx = v >> 24;
    uint32_t c = SVC_CONST;
    if (g_reply_queue)
        xQueueSend(g_respq[idx], &c, 0);
    else
        xTaskNotify(g_client_h[idx], c, eSetValueWithOverwrite);
}

static void svc_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t v;
        if (xQueueReceive(g_reqq, &v, g_tmo) != pdTRUE) continue;  // timeout: wait again
        reply(v);
    }
}

static void client_task(void *arg) {
    uint32_t idx = (uint32_t)(uintptr_t)arg;
    uint32_t *s = &g_samp[idx * ROW];
    uint32_t i = 0, r = 0;
    for (; i < ROW; i++) {
        uint32_t v = (idx << 24) | (i & 0xffffffu);
        uint32_t t0 = cc();
        if (xQueueSend(g_reqq, &v, g_tmo) != pdTRUE) { g_err |= FLAG_TIMEOUT; break; }
        if (g_reply_queue) {
            if (xQueueReceive(g_respq[idx], &r, g_tmo) != pdTRUE) { g_err |= FLAG_TIMEOUT; break; }
        } else {
            if (xTaskNotifyWait(0, 0xffffffffu, &r, g_tmo) != pdTRUE) { g_err |= FLAG_TIMEOUT; break; }
        }
        uint32_t t1 = cc();
        if (r != SVC_CONST) g_err |= FLAG_BADVAL;
        s[i] = t1 - t0;
    }
    g_done_n[idx] = i;
    xTaskNotifyGive(g_ctl);
    vTaskSuspend(NULL);
}

static void bg_task(void *arg) {
    TickType_t d = (TickType_t)(uintptr_t)arg;
    for (;;) vTaskDelay(d);
}

// ---------------------------------------------------------------- helpers
static void ctl_drain(void) { while (ulTaskNotifyTake(pdTRUE, 0)) {} }

// Wait until `n` completion notifications arrived. Returns FLAG_TIMEOUT on
// the safety timeout.
static uint32_t ctl_wait(uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!ulTaskNotifyTake(pdFALSE, SAFETY_TICKS)) return FLAG_TIMEOUT;
    return 0;
}

static void queues_make(uint32_t C) {
    g_reqq = xQueueCreateStatic(16, sizeof(uint32_t), (uint8_t *)g_reqq_buf, &g_reqq_cb);
    for (uint32_t i = 0; i < C; i++)
        g_respq[i] = xQueueCreateStatic(4, sizeof(uint32_t), (uint8_t *)g_respq_buf[i],
                                        &g_respq_cb[i]);
}
static void queues_del(uint32_t C) {
    vQueueDelete(g_reqq);
    for (uint32_t i = 0; i < C; i++) vQueueDelete(g_respq[i]);
}

// svc_mode: 0 = all at PRIO_SVC_HI, 1 = all different (PRIO_SVC_HI + i),
//           2 = all at PRIO_CLIENT (same as clients)
// One run of conditions 1..4: D background sleepers, W services, C clients.
// Row 1..4 differ only in these parameters. aux = {D, W, C, tmo}.
static void run_rt(const char *name, uint32_t C, uint32_t W, uint32_t svc_mode, int reply_q,
                   TickType_t tmo, uint32_t D, uint32_t bg_base) {
    g_err = 0;
    g_tmo = tmo;
    g_reply_queue = reply_q;
    memset(g_done_n, 0, sizeof(g_done_n));
    ctl_drain();
    queues_make(C);
    for (uint32_t i = 0; i < D; i++)
        g_bg_h[i] = xTaskCreateStatic(bg_task, "bg", BG_STK, (void *)(uintptr_t)(bg_base + 3 * i),
                                      PRIO_BG, g_bg_stk[i], &g_bg_tcb[i]);
    if (D) vTaskDelay(3);  // let sleepers enter the delayed list
    for (uint32_t i = 0; i < W; i++) {
        UBaseType_t p = svc_mode == 0 ? PRIO_SVC_HI : svc_mode == 1 ? PRIO_SVC_HI + i : PRIO_CLIENT;
        g_svc_h[i] = xTaskCreateStatic(svc_task, "svc", STK, NULL, p, g_svc_stk[i], &g_svc_tcb[i]);
    }
    for (uint32_t i = 0; i < C; i++)
        g_client_h[i] = xTaskCreateStatic(client_task, "cl", STK, (void *)(uintptr_t)i,
                                          PRIO_CLIENT, g_cl_stk[i], &g_cl_tcb[i]);
    uint32_t flags = ctl_wait(C) ? (FLAG_TIMEOUT | FLAG_ABORTED) : 0;
    for (uint32_t i = 0; i < C; i++) vTaskDelete(g_client_h[i]);
    for (uint32_t i = 0; i < W; i++) vTaskDelete(g_svc_h[i]);
    for (uint32_t i = 0; i < D; i++) vTaskDelete(g_bg_h[i]);
    queues_del(C);
    flags |= g_err;

    // per-client means (fairness), then compact all post-warm samples.
    uint32_t total = 0, mmin = 0xffffffffu, mmax = 0;
    for (uint32_t c = 0; c < C; c++) {
        uint32_t k = g_done_n[c] > WARM ? g_done_n[c] - WARM : 0;
        uint32_t *src = &g_samp[c * ROW + WARM];
        uint64_t sum = 0;
        for (uint32_t i = 0; i < k; i++) sum += src[i];
        if (k) {
            uint32_t m = (uint32_t)(sum / k);
            if (m < mmin) mmin = m;
            if (m > mmax) mmax = m;
        }
        memmove(&g_samp[total], src, k * sizeof(uint32_t));
        total += k;
    }
    if (mmin == 0xffffffffu) mmin = 0;
    uint32_t aux[4] = {D, W, C, tmo};
    // aux for C>1 is overwritten below to carry client-mean spread.
    finish(name, C > 1 ? C : (W > 1 ? W : D), flags, aux, g_samp, total);
    if (C > 1) {
        RES.r[RES.count - 1].aux[0] = mmin;  // aux0/1: min / max of per-client mean (raw)
        RES.r[RES.count - 1].aux[1] = mmax;
        RES.r[RES.count - 1].aux[2] = C;
    }
}

// ---------------------------------------------------------------- cond 5
static volatile TaskHandle_t g_wk_client;

static void worker_task(void *arg) {
    (void)arg;
    // no-op service: ignore value, return constant by notification
    xTaskNotify(g_wk_client, SVC_CONST, eSetValueWithOverwrite);
    vTaskSuspend(NULL);
}

static void create_client(void *arg) {
    UBaseType_t wprio = (UBaseType_t)(uintptr_t)arg;
    g_wk_client = xTaskGetCurrentTaskHandle();
    uint32_t i = 0;
    for (; i < ROW; i++) {
        uint32_t t0 = cc();
        TaskHandle_t w = xTaskCreateStatic(worker_task, "wk", STK, NULL, wprio, g_wk_stk, &g_wk_tcb);
        uint32_t t1 = cc();
        uint32_t r = 0;
        if (xTaskNotifyWait(0, 0xffffffffu, &r, TMO_FINITE) != pdTRUE) { g_err |= FLAG_TIMEOUT; vTaskDelete(w); break; }
        uint32_t t2 = cc();
        vTaskDelete(w);
        uint32_t t3 = cc();
        if (r != SVC_CONST) g_err |= FLAG_BADVAL;
        g_samp[i] = t3 - t0;
        g_seg[0][i] = t1 - t0;
        g_seg[1][i] = t2 - t1;
        g_seg[2][i] = t3 - t2;
    }
    g_done_n[0] = i;
    xTaskNotifyGive(g_ctl);
    vTaskSuspend(NULL);
}

static void run_create(const char *tag, UBaseType_t wprio) {
    g_err = 0;
    g_done_n[0] = 0;
    ctl_drain();
    g_client_h[0] = xTaskCreateStatic(create_client, "cl", STK, (void *)(uintptr_t)wprio,
                                      PRIO_CLIENT, g_cl_stk[0], &g_cl_tcb[0]);
    uint32_t flags = ctl_wait(1) ? (FLAG_TIMEOUT | FLAG_ABORTED) : 0;
    vTaskDelete(g_client_h[0]);
    flags |= g_err;
    uint32_t k = g_done_n[0] > WARM ? g_done_n[0] - WARM : 0;
    static const char *const part[4] = {"total", "create", "wait", "delete"};
    char nm[NAME_LEN];
    snprintf(nm, sizeof(nm), "c5_%s_%s", tag, part[0]);
    finish(nm, wprio, flags, NULL, &g_samp[WARM], k);
    for (int p = 1; p < 4; p++) {
        snprintf(nm, sizeof(nm), "c5_%s_%s", tag, part[p]);
        finish(nm, wprio, flags, NULL, &g_seg[p - 1][WARM], k);
    }
}

// ---------------------------------------------------------------- cond 6
typedef struct {
    volatile uint32_t req_seq, req_val, rsp_seq, rsp_val;
} mailbox_t;
static mailbox_t g_mb;
static int g_poll_yield;
static uint32_t g_poll_total;
static volatile uint32_t g_poll_done;

static void poll_svc(void *arg) {
    (void)arg;
    uint32_t last = 0;
    for (;;) {
        while (g_mb.req_seq == last)
            if (g_poll_yield) taskYIELD();
        last = g_mb.req_seq;
        __sync_synchronize();
        (void)g_mb.req_val;
        g_mb.rsp_val = SVC_CONST;
        __sync_synchronize();
        g_mb.rsp_seq = last;
    }
}

static void poll_client(void *arg) {
    (void)arg;
    uint32_t i = 0;
    for (; i < g_poll_total; i++) {
        uint32_t t0 = cc();
        g_mb.req_val = i;
        __sync_synchronize();
        g_mb.req_seq = i + 1;
        while (g_mb.rsp_seq != i + 1)
            if (g_poll_yield) taskYIELD();
        __sync_synchronize();
        uint32_t r = g_mb.rsp_val;
        uint32_t t1 = cc();
        if (r != SVC_CONST) g_err |= FLAG_BADVAL;
        g_samp[i] = t1 - t0;
        g_poll_done = i + 1;
    }
    xTaskNotifyGive(g_ctl);
    vTaskSuspend(NULL);
}

static void run_poll(const char *name, int yield, UBaseType_t svc_prio, uint32_t warm,
                     uint32_t total) {
    g_err = 0;
    g_poll_yield = yield;
    g_poll_total = total;
    g_poll_done = 0;
    memset((void *)&g_mb, 0, sizeof(g_mb));
    ctl_drain();
    g_svc_h[0] = xTaskCreateStatic(poll_svc, "ps", STK, NULL, svc_prio, g_svc_stk[0], &g_svc_tcb[0]);
    g_client_h[0] = xTaskCreateStatic(poll_client, "pc", STK, NULL, PRIO_CLIENT, g_cl_stk[0], &g_cl_tcb[0]);
    uint32_t flags = ctl_wait(1) ? (FLAG_TIMEOUT | FLAG_ABORTED) : 0;
    uint32_t done = g_poll_done;  // snapshot before deleting
    vTaskDelete(g_client_h[0]);
    vTaskDelete(g_svc_h[0]);
    flags |= g_err;
    uint32_t k = done > warm ? done - warm : 0;
    uint32_t aux[4] = {done, warm, total, yield};
    finish(name, svc_prio, flags, aux, &g_samp[warm], k);
}

// ---------------------------------------------------------------- control
static void ctl_task(void *arg) {
    (void)arg;
    g_ctl = xTaskGetCurrentTaskHandle();
    RES.magic = 0x46524254u;
    RES.version = 1;
    RES.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    RES.tick_hz = configTICK_RATE_HZ;
    vTaskDelay(10);
    calibrate();

    // 1: basic round trip (no background tasks, infinite wait)
    run_rt("c1a_svc_hi_notify", 1, 1, 0, 0, portMAX_DELAY, 0, 0);
    run_rt("c1b_svc_eq_notify", 1, 1, 2, 0, portMAX_DELAY, 0, 0);
    run_rt("c1c_svc_hi_queue", 1, 1, 0, 1, portMAX_DELAY, 0, 0);

    // 2: D sleepers. before = sleepers wake before the 1000-tick timeout (new
    // timeout is inserted behind all D), after = they wake later (inserted in front).
    static const uint32_t Ds[4] = {0, 4, 16, 64};
    char nm[NAME_LEN];
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "c2_tmo_bgbefore_D%u", (unsigned)Ds[k]);
        run_rt(nm, 1, 1, 0, 0, TMO_FINITE, Ds[k], BG_BEFORE);
        snprintf(nm, sizeof(nm), "c2_tmo_bgafter_D%u", (unsigned)Ds[k]);
        run_rt(nm, 1, 1, 0, 0, TMO_FINITE, Ds[k], BG_AFTER);
        snprintf(nm, sizeof(nm), "c2_max_D%u", (unsigned)Ds[k]);
        run_rt(nm, 1, 1, 0, 0, portMAX_DELAY, Ds[k], BG_BEFORE);
    }

    // 3: W services on one queue
    static const uint32_t Ws[3] = {1, 4, 16};
    for (int k = 0; k < 3; k++) {
        snprintf(nm, sizeof(nm), "c3_prio_same_W%u", (unsigned)Ws[k]);
        run_rt(nm, 1, Ws[k], 0, 0, portMAX_DELAY, 0, 0);
        snprintf(nm, sizeof(nm), "c3_prio_diff_W%u", (unsigned)Ws[k]);
        run_rt(nm, 1, Ws[k], 1, 0, portMAX_DELAY, 0, 0);
    }

    // 4: C clients on one service. hi = service above clients, eq = same priority.
    static const uint32_t Cs[4] = {1, 2, 4, 8};
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "c4_svc_hi_C%u", (unsigned)Cs[k]);
        run_rt(nm, Cs[k], 1, 0, 0, portMAX_DELAY, 0, 0);
    }
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "c4_svc_eq_C%u", (unsigned)Cs[k]);
        run_rt(nm, Cs[k], 1, 2, 0, portMAX_DELAY, 0, 0);
    }

    // 5: task per request
    run_create("wk_hi", PRIO_SVC_HI);
    run_create("wk_eq", PRIO_CLIENT);

    // 6: polling
    run_poll("c6a_poll_eq_yield", 1, PRIO_CLIENT, WARM, ROW);
    run_poll("c6b_poll_eq_noyield", 0, PRIO_CLIENT, 8, 72);  // 64 samples
    run_poll("c6c_poll_hi_noyield", 0, PRIO_SVC_HI, WARM, ROW);  // expected abort

    RES.done = 1;
    for (;;) {
        printf("DONE ovh=%u count=%u\n", (unsigned)RES.ovh, (unsigned)RES.count);
        for (uint32_t i = 0; i < RES.count; i++) print_result(&RES.r[i]);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void) {
    xTaskCreate(ctl_task, "ctl", 6144, NULL, PRIO_CTL, NULL);
}
