#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_app_desc.h"
#include "esp_private/startup_internal.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl_os.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "trap_api.hpp"
#include "freertos/FreeRTOS.h"
#include "compat.hpp"
#include "shizuku_port.hpp"
#include "soc/soc_memory_layout.h"
#include <cstdint>
#include <cstring>
#include <new>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

namespace shizuku::boards::esp32_c5 { uint64_t time_us(); }

namespace {
constexpr uint32_t kHeapRegionDiagCapacity = 16;
enum class heap_trace_phase : uint32_t { off = 0, heap_init = 1, controller_init = 2 };
volatile uint32_t g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::off);
}

extern "C" {
struct shizuku_c5_heap_region_diag_t {
  uint32_t start;
  uint32_t end;
  uint32_t iram_alias;
  uint32_t caps;
  uint32_t flags; // bit0 startup_stack, bit1 INTERNAL, bit2 overlaps static-data reserve
};
struct shizuku_c5_ble_task_diag_t {
  uint32_t task_id;
  char name[16];
  uint32_t create_count;
  uint32_t start_count;
};
struct shizuku_c5_ble_wait_diag_t {
  uint32_t task_id;
  uint32_t wait_kind; // high16: API kind (1 notify-take .. 9 delay), low16: object kind (1 queue .. 5 timer; 0 n/a)
  uint32_t handle;
  uint32_t timeout_ticks;
  uint32_t entries;
  uint32_t returns;
  uint32_t last_result;
};

struct shizuku_c5_ble_diag_t {
  uint32_t magic;
  uint32_t stage;
  int32_t controller_init_rc;
  int32_t controller_enable_rc;
  int32_t compat_bind_rc;
  uint32_t shim_timeout_addr;
  uint32_t idf_timeout_addr;
  uint32_t shim_queue_semaphore_addr;
  uint32_t shim_timeout_calls;
  uint32_t shim_queue_semaphore_calls;
  uint32_t shim_critical_calls;
  uint32_t shim_critical_addr;
  uint32_t idf_critical_addr;
  uint32_t first_unimplemented_api;
  uint32_t unimplemented_api_calls;
  uint32_t heap_init_state; // 0 not started, 1 entered, 2 initialized, 3 already initialized, >=4 fail-closed
  uint32_t heap_total_before;
  uint32_t heap_free_before;
  uint32_t heap_largest_before;
  uint32_t heap_total_after;
  uint32_t heap_free_after;
  uint32_t heap_largest_after;
  uint32_t heap_region_capacity;
  uint32_t heap_region_count;
  uint32_t heap_region_overflow;
  uint32_t heap_region_overlap;
  uint32_t static_data_start;
  uint32_t static_data_end;
  uint32_t heap_init_malloc_calls;
  uint32_t heap_init_malloc_requested;
  uint32_t heap_init_malloc_succeeded;
  uint32_t controller_heap_free_before;
  uint32_t controller_heap_largest_before;
  uint32_t controller_malloc_calls;
  uint32_t controller_malloc_requested;
  uint32_t controller_malloc_succeeded;
  uint32_t controller_malloc_failures;
  uint32_t controller_malloc_first_failed_size;
  uint32_t controller_malloc_first_failed_caps;
  uint32_t controller_heap_free_after;
  uint32_t controller_heap_largest_after;
  uint32_t enable_heap_free_after;
  uint32_t enable_heap_largest_after;
  shizuku_c5_heap_region_diag_t heap_regions[kHeapRegionDiagCapacity];
  uint32_t idf_init_table_count;
  uint32_t idf_init_error;
  uint32_t idf_init_current_slot;
  uint32_t idf_core_ran_mask;
  uint32_t idf_secondary_ran_mask;
  uint32_t idf_skipped_mask;
  int32_t idf_init_last_rc;
  uint32_t libc_ctors_state;
  int32_t nvs_init_rc;
  uint32_t libc_lock_mutex_handle;
  uint32_t sync_create_attempts[5]; // mutex, semaphore, queue, event group, timer
  uint32_t sync_create_failures[5];
  uint32_t sync_failure_reasons[7]; // no domain, ISR, object limit, invalid, handle map, mutex pool, unsupported caps
  uint32_t sync_last_kind;
  uint32_t sync_last_reason;
  uint32_t sync_objects_used;
  uint32_t sync_static_handles_used;
  uint32_t sync_dynamic_mutex_slots_used;
  uint32_t sync_last_queue_capacity;
  uint32_t sync_last_queue_item_size;
  uint32_t sync_last_queue_requested_bytes;
  uint32_t task_create_attempts;
  uint32_t task_create_successes;
  uint32_t task_create_failures;
  uint32_t task_start_total;
  uint32_t esp_timer_task_id;
  uint32_t esp_timer_task_create_count;
  uint32_t esp_timer_task_start_count;
  uint32_t task_diag_count;
  uint32_t task_diag_overflow;
  uint32_t notify_api_calls[7];
  uint32_t notify_api_no_delivery[7];
  shizuku_c5_ble_task_diag_t task_diag[8];
  shizuku_c5_ble_wait_diag_t wait_diag[10]; // root, eight compat tasks, overflow
  uint32_t last_queue_sem_caller_pc;
  int32_t nimble_port_init_rc;
  int32_t gap_device_name_rc;
  int32_t gatt_count_cfg_rc;
  int32_t gatt_add_svcs_rc;
  int32_t adv_addr_infer_rc;
  int32_t adv_set_fields_rc;
  int32_t adv_start_rc;
  uint32_t nimble_host_task_create_count;
  uint32_t nimble_host_task_start_count;
  uint32_t nimble_host_task_id;
  uint32_t nimble_sync_count;
  uint32_t nimble_reset_count;
  int32_t nimble_last_reset_reason;
  uint32_t ble_connect_count;
  uint32_t ble_disconnect_count;
  uint32_t ble_connection_handle;
  uint32_t gatt_read_count;
  uint32_t nimble_host_returned;
  uint32_t queue_send_count;
  uint32_t queue_send_last_handle;
  uint32_t queue_send_last_item_size;
  uint32_t queue_send_last_first_word;
  uint32_t queue_receive_count;
  uint32_t queue_receive_last_handle;
  uint32_t queue_receive_last_item_size;
  uint32_t queue_receive_last_first_word;
  uint32_t npl_host_funcs;
  uint32_t npl_controller_funcs;
  uint32_t npl_host_eventq_get;
  uint32_t npl_controller_eventq_get;
  uint32_t npl_controller_eventq_is_empty;
  uint32_t npl_eventq_probe_calls;
  uint32_t npl_eventq_probe_arg;
  uint32_t npl_eventq_probe_timeout;
  uint32_t npl_eventq_probe_storage;
  uint32_t npl_eventq_probe_queue_handle;
  uint32_t npl_eventq_probe_return;
  uint32_t npl_eventq_probe_return_event;
  uint32_t npl_eventq_probe_function;
  uint32_t npl_eventq_probe_records[8][8];
  uint32_t queue_payload_log_count;
  uint32_t queue_payload_records[16][5];
};
static_assert(sizeof(shizuku_c5_ble_diag_t) == 1968);

// OpenOCD-readable word layout, 1968 bytes total (492 words):
// [0] magic='C5BL', [1] stage (1=early init, 4=NVS ready, 5=NimBLE init
// returned, 6=host sync, 7=GATT ready after sync, 8=advertising,
// 9=connected, 10=GATT read; 0x20/0x30/0x31 are earlier init failures,
// 0x35/0x36/0x37/0x38 are NimBLE/GAP/GATT/host-or-adv failures),
// [2..3] controller init/enable
// outcomes are set to ESP_OK when nimble_port_init succeeds (NimBLE owns
// those calls in this profile),
// [4] FreeRTOS-shim PLAIN-route bind rc (pdPASS=1).
// [5..7] shim/IDF timeout and queue-semaphore function addresses;
// [8..10] shim wrapper hit counters; [11..12] shim/IDF critical addresses;
// [13] first unimplemented API id (0=none); [14] total fail-closed calls.
// [15..42] heap-init/controller heap snapshots and malloc trace;
// [43..122] sixteen {start,end,iram_alias,caps,flags} available-region records.
// [123..132] init table/CORE/SECONDARY/constructor/NVS/libc-lock diagnostics.
// [133..154] sync create attempts/failures by kind, failure reasons, last
// failure, and current domain/handle-table occupancy; [155..157] queue shape;
// [158..180] task/notification counters; [181..236] eight task records of
// {id,16-byte name,create_count,start_count}; [237..306] ten wait records of
// {task_id,kind,handle,timeout,entries,returns,last_result}
// for root, eight tracked tasks, and overflow tasks; [307] caller PC for the
// most recent xQueueSemaphoreTake wait entry; [308..325] NimBLE init/GAP/GATT/
// advertising rc, host task counters/id, sync/reset/connection/read counters,
// and whether the host loop returned; [326..333] last queue send/receive
// counts, raw handles, item sizes and first words. [334..345] capture host and
// controller NPL tables plus a transparent eventq_get probe; [346] is the
// installed probe function address; [347..410] are eight callback records
// {sequence,task_handle,eventq_arg,timeout,storage,queue_handle,result,event};
// [411] is queue payload log count; [412..491] are 16 records of
// {sequence,operation(1=send,2=receive),handle,item_size,first_word}.
// Values 0x7fffffff mean the corresponding call has not returned.
#if defined(__riscv)
__attribute__((used, aligned(4), section(".noinit")))
#else
__attribute__((used, aligned(4)))
#endif
volatile shizuku_c5_ble_diag_t shizuku_c5_ble_diag;
// Per-object live state, indexed by compat object id: kind, creator PC, owner,
// recursion, count, waiter count, last taker, last giver, take/give totals,
// highest-priority waiter, owner task state. This remains separate to keep the
// stable main diag compact.
volatile uint32_t shizuku_c5_sync_object_diag[24][14];
// address/value/kind/caller-PC for the most recent esp_libc lock acquire.
volatile uint32_t shizuku_c5_libc_lock_diag[4];
// last set state, last getter return, getter count, slot of last query,
// scheduler state immediately before/after CORE slot 20.
volatile uint32_t shizuku_c5_scheduler_diag[6];
volatile uint32_t shizuku_c5_ble_timeout_shim_calls = 0;
volatile uint32_t shizuku_c5_ble_queue_sem_shim_calls = 0;
volatile uint32_t shizuku_c5_ble_critical_shim_calls = 0;
extern struct npl_funcs_t *r_npl_funcs;
}

namespace {
constexpr char kBleDeviceName[] = "Shizuku-C5";
constexpr uint16_t kShizukuGattServiceUuid = 0xFFF0;
constexpr uint16_t kShizukuGattBuildVersionUuid = 0xFFF1;
constexpr uint32_t kBleCompatGrantCycles = 16000u; // 1 ms at the C5 16 MHz timer rate.
static ble_uuid16_t g_service_uuid{};
static ble_uuid16_t g_build_version_uuid{};
static ble_gatt_chr_def g_gatt_characteristics[2]{};
static ble_gatt_svc_def g_gatt_services[2]{};
static uint16_t g_build_version_value_handle = 0;
static struct ble_npl_event *(*g_npl_eventq_get_original)(
    struct ble_npl_eventq *, ble_npl_time_t) = nullptr;

bool shizuku_c5_ble_is_internal_ram(uintptr_t address) {
    return (address >= 0x40800000u && address < 0x40860000u) ||
           (address >= 0x50000000u && address < 0x50004000u);
}

extern "C" struct ble_npl_event *shizuku_c5_ble_probe_eventq_get(
    struct ble_npl_eventq *queue, ble_npl_time_t timeout) {
    auto &diag = shizuku_c5_ble_diag;
    const uint32_t sequence = __atomic_add_fetch(
        &diag.npl_eventq_probe_calls, 1u, __ATOMIC_RELAXED);
    volatile uint32_t *record = diag.npl_eventq_probe_records[(sequence - 1u) % 8u];
    record[0] = sequence;
    record[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        xTaskGetCurrentTaskHandle()));
    record[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue));
    record[3] = timeout;
    record[4] = 0;
    record[5] = 0;
    record[6] = UINT32_MAX;
    record[7] = UINT32_MAX;
    diag.npl_eventq_probe_arg = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(queue));
    diag.npl_eventq_probe_timeout = timeout;
    diag.npl_eventq_probe_storage = 0;
    diag.npl_eventq_probe_queue_handle = 0;
    diag.npl_eventq_probe_return = UINT32_MAX;
    diag.npl_eventq_probe_return_event = UINT32_MAX;
    if (queue && shizuku_c5_ble_is_internal_ram(reinterpret_cast<uintptr_t>(queue))) {
        auto *storage = static_cast<volatile uint32_t *>(queue->eventq);
        const uintptr_t storage_address = reinterpret_cast<uintptr_t>(storage);
        if (storage && shizuku_c5_ble_is_internal_ram(storage_address)) {
            diag.npl_eventq_probe_storage = static_cast<uint32_t>(storage_address);
            diag.npl_eventq_probe_queue_handle = storage[0];
            record[4] = static_cast<uint32_t>(storage_address);
            record[5] = storage[0];
        }
    }
    auto *original = g_npl_eventq_get_original;
    if (!original) return nullptr;
    struct ble_npl_event *event = original(queue, timeout);
    const uint32_t event_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event));
    diag.npl_eventq_probe_return = event_address;
    record[6] = event_address;
    if (event && shizuku_c5_ble_is_internal_ram(reinterpret_cast<uintptr_t>(event)))
    {
        const uint32_t event_payload =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event->event));
        diag.npl_eventq_probe_return_event = event_payload;
        record[7] = event_payload;
    }
    asm volatile("fence rw,rw" ::: "memory");
    return event;
}

void shizuku_c5_ble_install_npl_probe() {
    auto *host = npl_funcs;
    auto *controller = r_npl_funcs;
    shizuku_c5_ble_diag.npl_host_funcs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host));
    shizuku_c5_ble_diag.npl_controller_funcs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller));
    if (host) shizuku_c5_ble_diag.npl_host_eventq_get =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host->p_ble_npl_eventq_get));
    if (controller) {
        shizuku_c5_ble_diag.npl_controller_eventq_get =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller->p_ble_npl_eventq_get));
        shizuku_c5_ble_diag.npl_controller_eventq_is_empty =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller->p_ble_npl_eventq_is_empty));
        if (controller->p_ble_npl_eventq_get) {
            g_npl_eventq_get_original = controller->p_ble_npl_eventq_get;
            controller->p_ble_npl_eventq_get = shizuku_c5_ble_probe_eventq_get;
            shizuku_c5_ble_diag.npl_eventq_probe_function =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                    shizuku_c5_ble_probe_eventq_get));
        }
    }
    asm volatile("fence rw,rw" ::: "memory");
}

int shizuku_c5_ble_gap_event(struct ble_gap_event *event, void *arg);

void shizuku_c5_ble_set_stage(uint32_t stage) {
    if (shizuku_c5_ble_diag.stage < 0x30 &&
        shizuku_c5_ble_diag.stage < stage)
        shizuku_c5_ble_diag.stage = stage;
    asm volatile("fence rw,rw" ::: "memory");
}

[[noreturn]] void shizuku_c5_ble_cooperative_wait(void) {
    for (;;) {
        // The root Shizuku thread owns no implicit FreeRTOS time slice. Run
        // compat tasks explicitly; this grants each READY task for a bounded
        // 1 ms quantum and lets blocked tasks wake through SLEEP_US/timer IRQs.
        shizuku_freertos_compat_run_scheduler(kBleCompatGrantCycles);
        // If the compat domain was never bound, keep the boot-failure path
        // cooperative too. With a bound domain, run_scheduler does not return.
        (void)shizuku::freertos_compat::shizuku_port::call(
            shizuku::object_api::SLEEP_US, 1000u);
    }
}

int shizuku_c5_ble_gatt_access(uint16_t, uint16_t,
                               struct ble_gatt_access_ctxt *ctxt, void *) {
    if (!ctxt || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    const esp_app_desc_t *description = esp_app_get_description();
    if (!description) return BLE_ATT_ERR_UNLIKELY;
    size_t version_length = 0;
    while (version_length < sizeof(description->version) &&
           description->version[version_length] != '\0')
        ++version_length;
    if (os_mbuf_append(ctxt->om, description->version, version_length) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    shizuku_c5_ble_diag.gatt_read_count =
        shizuku_c5_ble_diag.gatt_read_count + 1u;
    shizuku_c5_ble_set_stage(10);
    return 0;
}

int shizuku_c5_ble_register_gatt(void) {
    g_service_uuid.u.type = BLE_UUID_TYPE_16;
    g_service_uuid.value = kShizukuGattServiceUuid;
    g_build_version_uuid.u.type = BLE_UUID_TYPE_16;
    g_build_version_uuid.value = kShizukuGattBuildVersionUuid;

    g_gatt_characteristics[0].uuid = &g_build_version_uuid.u;
    g_gatt_characteristics[0].access_cb = shizuku_c5_ble_gatt_access;
    g_gatt_characteristics[0].flags = BLE_GATT_CHR_F_READ;
    g_gatt_characteristics[0].val_handle = &g_build_version_value_handle;
    g_gatt_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_gatt_services[0].uuid = &g_service_uuid.u;
    g_gatt_services[0].characteristics = g_gatt_characteristics;

    int rc = ble_gatts_count_cfg(g_gatt_services);
    shizuku_c5_ble_diag.gatt_count_cfg_rc = rc;
    if (rc != 0) return rc;
    rc = ble_gatts_add_svcs(g_gatt_services);
    shizuku_c5_ble_diag.gatt_add_svcs_rc = rc;
    return rc;
}

int shizuku_c5_ble_start_advertising(void) {
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    shizuku_c5_ble_diag.adv_addr_infer_rc = rc;
    if (rc != 0) {
        return rc;
    }

    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(kBleDeviceName));
    fields.name_len = sizeof(kBleDeviceName) - 1u;
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    shizuku_c5_ble_diag.adv_set_fields_rc = rc;
    if (rc != 0) return rc;

    struct ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &params,
                           shizuku_c5_ble_gap_event, nullptr);
    shizuku_c5_ble_diag.adv_start_rc = rc;
    if (rc == 0) shizuku_c5_ble_set_stage(8);
    return rc;
}

void shizuku_c5_ble_on_reset(int reason) {
    shizuku_c5_ble_diag.nimble_reset_count =
        shizuku_c5_ble_diag.nimble_reset_count + 1u;
    shizuku_c5_ble_diag.nimble_last_reset_reason = reason;
}

void shizuku_c5_ble_on_sync(void) {
    shizuku_c5_ble_diag.nimble_sync_count =
        shizuku_c5_ble_diag.nimble_sync_count + 1u;
    shizuku_c5_ble_set_stage(6);
    if (shizuku_c5_ble_diag.gatt_add_svcs_rc != 0) return;
    shizuku_c5_ble_set_stage(7);
    if (shizuku_c5_ble_start_advertising() != 0) {
        shizuku_c5_ble_diag.stage = 0x38;
        asm volatile("fence rw,rw" ::: "memory");
    }
}

int shizuku_c5_ble_gap_event(struct ble_gap_event *event, void *) {
    if (!event) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            shizuku_c5_ble_diag.ble_connect_count =
                shizuku_c5_ble_diag.ble_connect_count + 1u;
            shizuku_c5_ble_diag.ble_connection_handle = event->connect.conn_handle;
            shizuku_c5_ble_set_stage(9);
        } else {
            (void)shizuku_c5_ble_start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        shizuku_c5_ble_diag.ble_disconnect_count =
            shizuku_c5_ble_diag.ble_disconnect_count + 1u;
        shizuku_c5_ble_diag.ble_connection_handle = UINT32_MAX;
        (void)shizuku_c5_ble_start_advertising();
        return 0;
    default:
        return 0;
    }
}

void shizuku_c5_ble_host_task(void *) {
    shizuku_c5_ble_diag.nimble_host_task_start_count =
        shizuku_c5_ble_diag.nimble_host_task_start_count + 1u;
    shizuku_c5_ble_diag.nimble_host_task_id = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()));
    nimble_port_run();
    nimble_port_freertos_deinit();
    shizuku_c5_ble_diag.nimble_host_returned = 1;
}
}

extern "C" {
extern BaseType_t __wrap_xTaskCheckForTimeOut(TimeOut_t *, TickType_t *);
extern BaseType_t __real_xTaskCheckForTimeOut(TimeOut_t *, TickType_t *);
extern BaseType_t __wrap_xQueueSemaphoreTake(QueueHandle_t, TickType_t);
extern void __wrap_vPortEnterCritical(portMUX_TYPE *);
extern void __real_vPortEnterCritical(portMUX_TYPE *);
extern void *__real_heap_caps_malloc(size_t size, uint32_t caps);

void *__wrap_heap_caps_malloc(size_t size, uint32_t caps) {
    void *result = __real_heap_caps_malloc(size, caps);
    const auto phase = static_cast<heap_trace_phase>(g_heap_trace_phase);
    if (phase == heap_trace_phase::heap_init) {
        __atomic_fetch_add(&shizuku_c5_ble_diag.heap_init_malloc_calls, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&shizuku_c5_ble_diag.heap_init_malloc_requested,
                           static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        if (result) {
            __atomic_fetch_add(&shizuku_c5_ble_diag.heap_init_malloc_succeeded,
                               static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        }
    } else if (phase == heap_trace_phase::controller_init) {
        __atomic_fetch_add(&shizuku_c5_ble_diag.controller_malloc_calls, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&shizuku_c5_ble_diag.controller_malloc_requested,
                           static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        if (result) {
            __atomic_fetch_add(&shizuku_c5_ble_diag.controller_malloc_succeeded,
                               static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&shizuku_c5_ble_diag.controller_malloc_failures,
                               1u, __ATOMIC_RELAXED);
            uint32_t expected = 0;
            __atomic_compare_exchange_n(&shizuku_c5_ble_diag.controller_malloc_first_failed_size,
                                        &expected, static_cast<uint32_t>(size), false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            expected = 0;
            __atomic_compare_exchange_n(&shizuku_c5_ble_diag.controller_malloc_first_failed_caps,
                                        &expected, caps, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
    }
    return result;
}

void shizuku_c5_ble_note_unimplemented(uint32_t api_id) {
    uint32_t expected = 0;
    __atomic_compare_exchange_n(&shizuku_c5_ble_diag.first_unimplemented_api, &expected,
                                api_id, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&shizuku_c5_ble_diag.unimplemented_api_calls, 1u, __ATOMIC_RELAXED);
}

void shizuku_c5_ble_note_sync_alloc(uint32_t kind, uint32_t reason,
                                   uint32_t objects_used,
                                   uint32_t static_handles_used,
                                   uint32_t mutex_storage_used) {
    if (kind == 0 || kind > 5) return;
    __atomic_fetch_add(&shizuku_c5_ble_diag.sync_create_attempts[kind - 1], 1u, __ATOMIC_RELAXED);
    if (reason != 0) {
        __atomic_fetch_add(&shizuku_c5_ble_diag.sync_create_failures[kind - 1], 1u, __ATOMIC_RELAXED);
        if (reason <= 7)
            __atomic_fetch_add(&shizuku_c5_ble_diag.sync_failure_reasons[reason - 1], 1u, __ATOMIC_RELAXED);
    }
    shizuku_c5_ble_diag.sync_last_kind = kind;
    shizuku_c5_ble_diag.sync_last_reason = reason;
    shizuku_c5_ble_diag.sync_objects_used = objects_used;
    shizuku_c5_ble_diag.sync_static_handles_used = static_handles_used;
    shizuku_c5_ble_diag.sync_dynamic_mutex_slots_used = mutex_storage_used;
}

void shizuku_c5_ble_note_queue_shape(uint32_t capacity, uint32_t item_size) {
    shizuku_c5_ble_diag.sync_last_queue_capacity = capacity;
    shizuku_c5_ble_diag.sync_last_queue_item_size = item_size;
    const uint64_t requested = static_cast<uint64_t>(capacity) * item_size;
    shizuku_c5_ble_diag.sync_last_queue_requested_bytes =
        requested > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(requested);
}

void shizuku_c5_ble_note_queue_send(uint32_t handle, uint32_t item_size,
                                    uint32_t first_word) {
    shizuku_c5_ble_diag.queue_send_count = shizuku_c5_ble_diag.queue_send_count + 1u;
    shizuku_c5_ble_diag.queue_send_last_handle = handle;
    shizuku_c5_ble_diag.queue_send_last_item_size = item_size;
    shizuku_c5_ble_diag.queue_send_last_first_word = first_word;
    const uint32_t sequence = __atomic_add_fetch(
        &shizuku_c5_ble_diag.queue_payload_log_count, 1u, __ATOMIC_RELAXED);
    volatile uint32_t *record = shizuku_c5_ble_diag.queue_payload_records[
        (sequence - 1u) % 16u];
    record[0] = sequence;
    record[1] = 1;
    record[2] = handle;
    record[3] = item_size;
    record[4] = first_word;
}

void shizuku_c5_ble_note_queue_receive(uint32_t handle, uint32_t item_size,
                                       uint32_t first_word) {
    shizuku_c5_ble_diag.queue_receive_count = shizuku_c5_ble_diag.queue_receive_count + 1u;
    shizuku_c5_ble_diag.queue_receive_last_handle = handle;
    shizuku_c5_ble_diag.queue_receive_last_item_size = item_size;
    shizuku_c5_ble_diag.queue_receive_last_first_word = first_word;
    const uint32_t sequence = __atomic_add_fetch(
        &shizuku_c5_ble_diag.queue_payload_log_count, 1u, __ATOMIC_RELAXED);
    volatile uint32_t *record = shizuku_c5_ble_diag.queue_payload_records[
        (sequence - 1u) % 16u];
    record[0] = sequence;
    record[1] = 2;
    record[2] = handle;
    record[3] = item_size;
    record[4] = first_word;
}

static volatile shizuku_c5_ble_task_diag_t *shizuku_c5_ble_find_task_diag(uint32_t task_id) {
    for (auto &record : shizuku_c5_ble_diag.task_diag)
        if (record.task_id == task_id && task_id != 0) return &record;
    return nullptr;
}

static volatile shizuku_c5_ble_task_diag_t *shizuku_c5_ble_add_task_diag(uint32_t task_id) {
    if (!task_id) return nullptr;
    if (auto *found = shizuku_c5_ble_find_task_diag(task_id)) return found;
    for (auto &record : shizuku_c5_ble_diag.task_diag) {
        if (record.task_id == 0) {
            record.task_id = task_id;
            shizuku_c5_ble_diag.task_diag_count = shizuku_c5_ble_diag.task_diag_count + 1u;
            return &record;
        }
    }
    shizuku_c5_ble_diag.task_diag_overflow = shizuku_c5_ble_diag.task_diag_overflow + 1u;
    return nullptr;
}

void shizuku_c5_ble_note_task_create(uint32_t task_id, const char *name, uint32_t success) {
    shizuku_c5_ble_diag.task_create_attempts = shizuku_c5_ble_diag.task_create_attempts + 1u;
    if (!success) {
        shizuku_c5_ble_diag.task_create_failures = shizuku_c5_ble_diag.task_create_failures + 1u;
        return;
    }
    shizuku_c5_ble_diag.task_create_successes = shizuku_c5_ble_diag.task_create_successes + 1u;
    auto *record = shizuku_c5_ble_add_task_diag(task_id);
    if (record) {
        bool name_ended = name == nullptr;
        for (size_t i = 0; i < sizeof(record->name); ++i) {
            record->name[i] = name_ended ? '\0' : name[i];
            if (record->name[i] == '\0') name_ended = true;
        }
        record->create_count = record->create_count + 1u;
    }
    if (name && std::strcmp(name, "esp_timer") == 0) {
        shizuku_c5_ble_diag.esp_timer_task_id = task_id;
        shizuku_c5_ble_diag.esp_timer_task_create_count = shizuku_c5_ble_diag.esp_timer_task_create_count + 1u;
    }
}

void shizuku_c5_ble_note_task_started(uint32_t task_id) {
    shizuku_c5_ble_diag.task_start_total = shizuku_c5_ble_diag.task_start_total + 1u;
    auto *record = shizuku_c5_ble_add_task_diag(task_id);
    if (record) record->start_count = record->start_count + 1u;
    if (task_id && task_id == shizuku_c5_ble_diag.esp_timer_task_id)
        shizuku_c5_ble_diag.esp_timer_task_start_count = shizuku_c5_ble_diag.esp_timer_task_start_count + 1u;
}

void shizuku_c5_ble_note_notification(uint32_t api_kind, bool delivered) {
    if (api_kind == 0 || api_kind > 7) return;
    __atomic_fetch_add(&shizuku_c5_ble_diag.notify_api_calls[api_kind - 1], 1u, __ATOMIC_RELAXED);
    if (!delivered)
        __atomic_fetch_add(&shizuku_c5_ble_diag.notify_api_no_delivery[api_kind - 1], 1u, __ATOMIC_RELAXED);
}

void shizuku_c5_ble_note_wait(uint32_t task_id, uint32_t kind, uint32_t handle,
                             uint32_t timeout_ticks, uint32_t entering, uint32_t result) {
    size_t slot = 9;
    if (task_id != 0) {
        for (size_t i = 0; i < 8; ++i) {
            if (shizuku_c5_ble_diag.task_diag[i].task_id == task_id) { slot = i + 1; break; }
        }
    } else {
        slot = 0;
    }
    auto &wait = shizuku_c5_ble_diag.wait_diag[slot];
    wait.task_id = task_id;
    if (entering) {
        wait.wait_kind = kind;
        wait.handle = handle;
        wait.timeout_ticks = timeout_ticks;
        wait.entries = wait.entries + 1u;
    } else {
        wait.wait_kind = 0;
        wait.returns = wait.returns + 1u;
        wait.last_result = result;
    }
}

void shizuku_c5_ble_note_wait_callsite(uint32_t pc) {
    shizuku_c5_ble_diag.last_queue_sem_caller_pc = pc;
}

void shizuku_c5_ble_note_sync_object(uint32_t id, uint32_t kind, uint32_t creator_pc,
                                     uint32_t raw_handle,
                                     uint32_t recursive,
                                     uint32_t owner, uint32_t recursion, uint32_t count,
                                     uint32_t waiters, uint32_t last_taker,
                                     uint32_t last_giver, uint32_t take_count,
                                     uint32_t give_count, uint32_t highest_waiter,
                                     uint32_t owner_state) {
    if (id >= 24) return;
    volatile uint32_t *row = shizuku_c5_sync_object_diag[id];
    row[0] = kind;
    row[1] = creator_pc;
    row[2] = owner;
    row[3] = recursion;
    row[4] = count;
    row[5] = waiters;
    row[6] = last_taker;
    row[7] = last_giver;
    row[8] = take_count;
    row[9] = give_count;
    row[10] = highest_waiter;
    row[11] = owner_state;
    row[12] = raw_handle;
    row[13] = recursive;
}

void shizuku_c5_ble_note_libc_lock(uint32_t lock_address, uint32_t lock_value,
                                   uint32_t api_kind, uint32_t caller_pc) {
    shizuku_c5_libc_lock_diag[0] = lock_address;
    shizuku_c5_libc_lock_diag[1] = lock_value;
    shizuku_c5_libc_lock_diag[2] = api_kind;
    shizuku_c5_libc_lock_diag[3] = caller_pc;
}

void shizuku_c5_ble_note_scheduler(uint32_t event, uint32_t state) {
    if (event == 0) {
        shizuku_c5_scheduler_diag[0] = state;
        return;
    }
    if (event != 1) return;
    shizuku_c5_scheduler_diag[1] = state;
    shizuku_c5_scheduler_diag[2] = shizuku_c5_scheduler_diag[2] + 1u;
    shizuku_c5_scheduler_diag[3] = shizuku_c5_ble_diag.idf_init_current_slot;
    if (shizuku_c5_ble_diag.idf_init_current_slot == 20)
        shizuku_c5_scheduler_diag[4] = state;
}

static bool shizuku_c5_ble_inspect_heap_regions(void) {
    extern uint8_t _data_start;
    extern uint8_t _heap_start;
    auto &d = shizuku_c5_ble_diag;
    d.heap_total_before = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    d.heap_free_before = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    d.heap_largest_before = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    d.static_data_start = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_data_start));
    d.static_data_end = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_heap_start));

    soc_memory_region_t regions[kHeapRegionDiagCapacity]{};
    const size_t max_regions = soc_get_available_memory_region_max_count();
    d.heap_region_capacity = static_cast<uint32_t>(max_regions);
    if (max_regions > kHeapRegionDiagCapacity) {
        d.heap_region_overflow = 1;
        d.heap_init_state = 4;
        return false;
    }
    const size_t count = soc_get_available_memory_regions(regions);
    d.heap_region_count = static_cast<uint32_t>(count);
    if (count > kHeapRegionDiagCapacity) {
        d.heap_region_overflow = 1;
        d.heap_init_state = 4;
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto &r = regions[i];
        if (r.type >= soc_memory_type_count) {
            d.heap_init_state = 4;
            return false;
        }
        const auto &type = soc_memory_types[r.type];
        const uint32_t caps = type.caps[0] | type.caps[1] | type.caps[2];
        const uint64_t end = static_cast<uint64_t>(r.start) + r.size;
        const bool overlaps = static_cast<uint64_t>(r.start) < d.static_data_end &&
                              end > d.static_data_start;
        auto &out = d.heap_regions[i];
        out.start = static_cast<uint32_t>(r.start);
        out.end = static_cast<uint32_t>(end);
        out.iram_alias = static_cast<uint32_t>(r.iram_address);
        out.caps = caps;
        out.flags = (r.startup_stack ? 1u : 0u) |
                    ((caps & MALLOC_CAP_INTERNAL) ? 2u : 0u) |
                    (overlaps ? 4u : 0u);
        if (overlaps) d.heap_region_overlap = 1;
    }
    if (d.heap_region_overlap) {
        d.heap_init_state = 5;
        return false;
    }

    return true;
}

extern "C" esp_system_init_fn_t _esp_sys_init_fn_start[];
extern "C" esp_system_init_fn_t _esp_sys_init_fn_end[];
extern "C" void *shizuku_freertos_compat_last_mutex_handle(void);
constexpr uint32_t kExpectedInitCount = 28;
constexpr uint32_t kCoreRunMask = (1u << 4) | (1u << 7) | (1u << 8) |
    (1u << 15) | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) |
    (1u << 20) | (1u << 21);
constexpr uint32_t kSecondaryRunMask = (1u << 5) | (1u << 9) |
    (1u << 23) | (1u << 24);

static bool shizuku_c5_ble_run_idf_stage(uint32_t stage) {
    auto &d = shizuku_c5_ble_diag;
    const size_t count = static_cast<size_t>(_esp_sys_init_fn_end - _esp_sys_init_fn_start);
    d.idf_init_table_count = static_cast<uint32_t>(count);
    if (count != kExpectedInitCount) { d.idf_init_error = 1; return false; }
    const uint32_t run_mask = stage == ESP_SYSTEM_INIT_STAGE_CORE ? kCoreRunMask : kSecondaryRunMask;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const auto &entry = _esp_sys_init_fn_start[slot];
        if (entry.stage != stage) continue;
        if ((run_mask & (1u << slot)) == 0) {
            d.idf_skipped_mask = d.idf_skipped_mask | (1u << slot);
            continue;
        }
        d.idf_init_current_slot = slot;
        if (slot == 20)
            shizuku_c5_scheduler_diag[4] = static_cast<uint32_t>(xTaskGetSchedulerState());
        if (slot == 4) {
            if (!shizuku_c5_ble_inspect_heap_regions()) { d.idf_init_error = 2; return false; }
            d.heap_init_state = 1;
            g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::heap_init);
        }
        const esp_err_t rc = entry.fn();
        if (slot == 20)
            shizuku_c5_scheduler_diag[5] = static_cast<uint32_t>(xTaskGetSchedulerState());
        if (slot == 8)
            d.libc_lock_mutex_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(shizuku_freertos_compat_last_mutex_handle()));
        if (slot == 4) {
            g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::off);
            d.heap_init_state = 2;
            d.heap_total_after = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
            d.heap_free_after = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            d.heap_largest_after = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (!d.heap_total_after) { d.heap_init_state = 6; d.idf_init_error = 3; return false; }
        }
        d.idf_init_last_rc = rc;
        if (rc != ESP_OK) { d.idf_init_error = 4; return false; }
        if (stage == ESP_SYSTEM_INIT_STAGE_CORE)
            d.idf_core_ran_mask = d.idf_core_ran_mask | (1u << slot);
        else
            d.idf_secondary_ran_mask = d.idf_secondary_ran_mask | (1u << slot);
    }
    d.idf_init_current_slot = UINT32_MAX;
    return true;
}

alignas(shizuku::freertos_compat::shizuku_port)
static unsigned char shizuku_c5_ble_port_storage[sizeof(shizuku::freertos_compat::shizuku_port)];
alignas(shizuku::freertos_compat::domain)
static unsigned char shizuku_c5_ble_domain_storage[sizeof(shizuku::freertos_compat::domain)];

extern "C" int shizuku_c5_ble_pre_core(void)
{
    constexpr uint32_t kMagic = 0x4C423543u; // bytes "C5BL" in little endian
    constexpr int32_t kNotReturned = 0x7fffffff;
    auto *diag_words = reinterpret_cast<volatile uint32_t *>(&shizuku_c5_ble_diag);
    for (uint32_t i = 0; i < sizeof(shizuku_c5_ble_diag_t) / sizeof(uint32_t); ++i) diag_words[i] = 0;
    for (uint32_t i = 0; i < 24; ++i)
        for (uint32_t j = 0; j < 14; ++j) shizuku_c5_sync_object_diag[i][j] = 0;
    for (uint32_t i = 0; i < 4; ++i) shizuku_c5_libc_lock_diag[i] = 0;
    for (uint32_t i = 0; i < 6; ++i) shizuku_c5_scheduler_diag[i] = 0;
    shizuku_c5_scheduler_diag[4] = UINT32_MAX;
    shizuku_c5_scheduler_diag[5] = UINT32_MAX;
    shizuku_c5_ble_diag.magic = kMagic;
    shizuku_c5_ble_diag.stage = 1;
    shizuku_c5_ble_diag.controller_init_rc = kNotReturned;
    shizuku_c5_ble_diag.controller_enable_rc = kNotReturned;
    shizuku_c5_ble_diag.nimble_port_init_rc = kNotReturned;
    shizuku_c5_ble_diag.gap_device_name_rc = kNotReturned;
    shizuku_c5_ble_diag.gatt_count_cfg_rc = kNotReturned;
    shizuku_c5_ble_diag.gatt_add_svcs_rc = kNotReturned;
    shizuku_c5_ble_diag.adv_set_fields_rc = kNotReturned;
    shizuku_c5_ble_diag.adv_start_rc = kNotReturned;
    shizuku_c5_ble_diag.adv_addr_infer_rc = kNotReturned;
    shizuku_c5_ble_diag.ble_connection_handle = UINT32_MAX;
    shizuku_c5_ble_diag.compat_bind_rc = kNotReturned;
    shizuku_c5_ble_diag.idf_init_current_slot = UINT32_MAX;
    shizuku_c5_ble_diag.idf_init_last_rc = kNotReturned;
    shizuku_c5_ble_diag.nvs_init_rc = kNotReturned;
    shizuku_c5_ble_diag.shim_timeout_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_xTaskCheckForTimeOut));
    shizuku_c5_ble_diag.idf_timeout_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__real_xTaskCheckForTimeOut));
    shizuku_c5_ble_diag.shim_queue_semaphore_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_xQueueSemaphoreTake));
    shizuku_c5_ble_diag.shim_critical_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_vPortEnterCritical));
    shizuku_c5_ble_diag.idf_critical_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__real_vPortEnterCritical));
    shizuku_c5_ble_diag.shim_timeout_calls = 0;
    shizuku_c5_ble_diag.shim_queue_semaphore_calls = 0;
    shizuku_c5_ble_diag.shim_critical_calls = 0;
    shizuku_c5_ble_diag.first_unimplemented_api = 0;
    shizuku_c5_ble_diag.unimplemented_api_calls = 0;

    auto *port = new (shizuku_c5_ble_port_storage) shizuku::freertos_compat::shizuku_port{};
    auto *domain = new (shizuku_c5_ble_domain_storage) shizuku::freertos_compat::domain(*port);
    shizuku::freertos_compat::shizuku_port::set_clock(&shizuku::boards::esp32_c5::time_us);
    const BaseType_t bind_rc = shizuku_freertos_compat_bind(
        domain, 0, shizuku::freertos_compat::shizuku_port::kFirstPlainTaskObject,
        CONFIG_FREERTOS_HZ);
    shizuku_c5_ble_diag.compat_bind_rc = bind_rc;
    asm volatile("fence rw,rw" ::: "memory");
    if (bind_rc != pdPASS) {
        shizuku_c5_ble_diag.stage = 0x10;
        return 0;
    }

    shizuku::chips::esp32_c5::install_traps();
    if (!shizuku_c5_ble_run_idf_stage(ESP_SYSTEM_INIT_STAGE_CORE)) {
        shizuku_c5_ble_diag.stage = 0x20;
        asm volatile("fence rw,rw" ::: "memory");
        return 0;
    }
    shizuku_c5_ble_diag.libc_ctors_state = 1;
    return 1;
}

extern "C" void shizuku_c5_ble_post_core_ctors(void) {
    shizuku_c5_ble_diag.libc_ctors_state = 2;
}

extern "C" [[noreturn]] void shizuku_c5_ble_stage1_start(void)
{
    // CORE callbacks and libc constructors run before the Shizuku task layer
    // is ready. Keep libc locks as pre-scheduler no-ops until SECONDARY init.
    shizuku_freertos_compat_set_scheduler_state(taskSCHEDULER_RUNNING);
    if (shizuku_c5_ble_diag.compat_bind_rc != pdPASS ||
        shizuku_c5_ble_diag.idf_init_error != 0) {
        shizuku_c5_ble_cooperative_wait();
    }
    if (!shizuku_c5_ble_run_idf_stage(ESP_SYSTEM_INIT_STAGE_SECONDARY)) {
        shizuku_c5_ble_diag.stage = 0x30;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }
    const esp_err_t nvs_rc = nvs_flash_init();
    shizuku_c5_ble_diag.nvs_init_rc = static_cast<int32_t>(nvs_rc);
    if (nvs_rc != ESP_OK) {
        shizuku_c5_ble_diag.stage = 0x31;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }
    shizuku_c5_ble_set_stage(4);
    shizuku_c5_ble_diag.controller_heap_free_before =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_c5_ble_diag.controller_heap_largest_before =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::controller_init);
    // IDF v6.1 owns controller init/enable and HCI setup inside
    // nimble_port_init(); calling esp_bt_controller_init/enable separately
    // first is a duplicate-init error on the NimBLE path.
    const esp_err_t init_rc = nimble_port_init();
    g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::off);
    shizuku_c5_ble_diag.nimble_port_init_rc = static_cast<int32_t>(init_rc);
    shizuku_c5_ble_diag.controller_heap_free_after =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_c5_ble_diag.controller_heap_largest_after =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    shizuku_c5_ble_diag.enable_heap_free_after =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_c5_ble_diag.enable_heap_largest_after =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (init_rc != ESP_OK) {
        shizuku_c5_ble_diag.stage = 0x35;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }

    // nimble_port_init() returns ESP_OK only after its internal controller
    // init/enable and HCI bring-up complete. Their individual internal return
    // values are not exposed by this IDF API.
    shizuku_c5_ble_diag.controller_init_rc = ESP_OK;
    shizuku_c5_ble_diag.controller_enable_rc = ESP_OK;
    // Capture and transparently probe the controller's registered NPL table.
    // This makes the next controller-side eventq_get result observable before
    // r_ble_ll_reset dereferences it, without changing the returned pointer.
    shizuku_c5_ble_install_npl_probe();
    shizuku_c5_ble_set_stage(5);
    ble_hs_cfg.reset_cb = shizuku_c5_ble_on_reset;
    ble_hs_cfg.sync_cb = shizuku_c5_ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    const int name_rc = ble_svc_gap_device_name_set(kBleDeviceName);
    shizuku_c5_ble_diag.gap_device_name_rc = name_rc;
    if (name_rc != 0) {
        shizuku_c5_ble_diag.stage = 0x36;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }

    const int gatt_rc = shizuku_c5_ble_register_gatt();
    if (gatt_rc != 0) {
        shizuku_c5_ble_diag.stage = 0x37;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }

    const uint32_t host_tasks_before = shizuku_c5_ble_diag.task_create_successes;
    nimble_port_freertos_init(shizuku_c5_ble_host_task);
    shizuku_c5_ble_diag.nimble_host_task_create_count =
        shizuku_c5_ble_diag.task_create_successes - host_tasks_before;
    if (shizuku_c5_ble_diag.nimble_host_task_create_count == 0) {
        shizuku_c5_ble_diag.stage = 0x38;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_c5_ble_cooperative_wait();
    }
    shizuku_c5_ble_cooperative_wait();
}
}
