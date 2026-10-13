#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_app_desc.h"
#include "esp_private/startup_internal.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl_os.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "trap_api.hpp"
#include "ble_chip.hpp"
#include "freertos/FreeRTOS.h"
#include "compat.hpp"
#include "shizuku_port.hpp"
#include "soc/soc_memory_layout.h"
#include "soc/interrupts.h"
#include <cstdint>
#include <cstring>
#include <new>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

extern "C" BaseType_t shizuku_freertos_compat_set_ble_scheduler_boost(
    uint32_t task_id, uint32_t enabled, uint32_t priority_boost,
    uint32_t idle_poll_us);

namespace {
constexpr uint32_t kHeapRegionDiagCapacity = 16;
enum class heap_trace_phase : uint32_t { off = 0, heap_init = 1, controller_init = 2 };
volatile uint32_t g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::off);
}

extern "C" {
struct shizuku_esp_heap_region_diag_t {
  uint32_t start;
  uint32_t end;
  uint32_t iram_alias;
  uint32_t caps;
  uint32_t flags; // bit0 startup_stack, bit1 INTERNAL, bit2 overlaps static-data reserve
};
struct shizuku_esp_ble_task_diag_t {
  uint32_t task_id;
  char name[16];
  uint32_t create_count;
  uint32_t start_count;
};
struct shizuku_esp_ble_wait_diag_t {
  uint32_t task_id;
  uint32_t wait_kind; // high16: API kind (1 notify-take .. 9 delay), low16: object kind (1 queue .. 5 timer; 0 n/a)
  uint32_t handle;
  uint32_t timeout_ticks;
  uint32_t entries;
  uint32_t returns;
  uint32_t last_result;
};

struct shizuku_esp_ble_diag_t {
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
  shizuku_esp_heap_region_diag_t heap_regions[kHeapRegionDiagCapacity];
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
  shizuku_esp_ble_task_diag_t task_diag[8];
  shizuku_esp_ble_wait_diag_t wait_diag[10]; // root, eight compat tasks, overflow
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
  uint32_t adv_restart_count;
  int32_t adv_restart_last_rc;
  int32_t ble_connect_fail_status;
  int32_t ble_disconnect_reason;
  int32_t adv_callout_init_rc;
  int32_t adv_retry_schedule_rc;
  uint32_t ble_conn_duration_us;
  uint32_t ble_conn_irq_registration_count;
  uint32_t ble_conn_irq_total_delta;
  uint32_t ble_conn_irq_sources[16];
  uint32_t ble_conn_irq_dispatch_deltas[16];
  uint32_t sched_task_priority[8];
  uint32_t sched_task_ready_count[8];
  uint32_t sched_task_last_wake_to_run_us[8];
  uint32_t sched_task_max_wake_to_run_us[8];
  int32_t ble_conn_params_find_rc;
  int32_t ble_conn_params_update_rc;
  int32_t ble_conn_params_update_status;
  uint32_t ble_conn_interval;
  uint32_t ble_conn_latency;
  uint32_t ble_conn_supervision_timeout;
  uint32_t sched_latency_clock_skew_count;
  uint32_t ble_conn_pend_switch_delta;
  uint32_t ble_irq_source4_dispatch_count;
  uint32_t ble_irq_source7_dispatch_count;
  int32_t ble_default_phy_rc;
  int32_t ble_conn_data_len_rc;
  uint32_t ble_security_profile;
  uint32_t ble_scheduler_boost_apply_count;
  uint32_t ble_scheduler_boost_active;
  uint32_t ble_conn_nimble_host_max_wake_to_run_us;
  uint32_t ble_event_sequence;
  uint32_t ble_event_count;
  uint32_t ble_event_records[24][9];
  uint32_t ble_conn_bt_irq_gap_max_us;
  uint32_t ble_conn_bt_irq_gap_max_elapsed_us;
  uint32_t ble_conn_bt_irq_gap_max_prior_mepc;
  uint32_t ble_conn_bt_irq_gap_max_prior_task;
  uint32_t ble_conn_bt_irq_gap_histogram[8];
  uint32_t ble_conn_bt_irq_gap_ring_head;
  uint32_t ble_conn_bt_irq_gap_ring_count;
  uint32_t ble_conn_bt_irq_gap_records[4][5]; // elapsed,gap,prior mepc,prior task,end kind
  uint32_t ble_conn_disconnect_elapsed_us;
  uint32_t ble_irq_mask_max_us[5];
  uint32_t ble_irq_mask_max_pc[5];
  uint32_t ble_irq_mask_max_task[5];
  uint32_t ble_irq_mask_count[5];
  uint32_t ble_cfg_bt_le_sleep_enable;
  uint32_t ble_cfg_bt_le_lp_clk_src; // 0=RTC slow, 1=main XTAL, 2=other/unknown
  uint32_t ble_cfg_rtc_slow_clk_src; // 0=external 32K, 1=136K RC, 2=other/unknown
  uint32_t ble_cfg_bt_le_sca_ppm;
  uint32_t ble_cfg_pm_enable;
  uint32_t ble_cfg_tickless_idle;
  uint32_t ble_cfg_nimble_use_esp_timer;
  uint32_t ble_cfg_pm_cpu_power_down;
  uint32_t ble_cfg_pm_peripheral_power_down;
  uint32_t ble_rssi_sample_count;
  uint32_t ble_rssi_valid_count;
  int32_t ble_rssi_min_dbm;
  int32_t ble_rssi_max_dbm;
  int32_t ble_rssi_last_dbm;
  int32_t ble_rssi_last_rc;
  int32_t ble_rssi_disconnect_dbm;
  int32_t ble_rssi_disconnect_rc;
  uint32_t ble_rssi_disconnect_elapsed_us;
  uint32_t ble_rssi_ring_head;
  uint32_t ble_rssi_records[8][3]; // query-start elapsed-us, signed dBm, rc
  int32_t ble_rssi_callout_init_rc;
  int32_t ble_rssi_callout_reset_rc;
  uint32_t ble_cfg_main_xtal_mhz;
  uint32_t ble_idf_default_bt_lpclk_hz;
  uint32_t ble_cfg_phy_cal_data_storage;
  uint32_t ble_cfg_phy_calibration_mode;
  uint32_t ble_tx_enabled;
  uint32_t ble_tx_notifications;
  uint32_t ble_tx_errors;
  int32_t ble_tx_last_notify_rc;
  uint32_t ble_tx_last_mtu;
  uint32_t ble_tx_last_payload_len;
  int32_t ble_tx_callout_init_rc;
};
static_assert(sizeof(shizuku_esp_ble_diag_t) == 3640);

// OpenOCD-readable word layout, 3640 bytes total (910 words):
// [0] magic='EBLE', [1] stage (1=early init, 4=NVS ready, 5=NimBLE init
// returned, 6=host sync, 7=GATT ready after sync, 8=advertising,
// 9=connected, 10=GATT read; 0x20/0x30/0x31 are earlier init failures,
// 0x34 is the C6 board RF GPIO setup failure,
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
// {sequence,operation(1=send,2=receive),handle,item_size,first_word};
// [492..493] advertising restart count and most recent return code;
// [494..497] failed-connect status, disconnect reason, callout-init rc, and
// most recent retry-schedule rc; [498..500] connection duration, current or
// connection IRQ registration count, and total IRQ delta; [501..516] raw IRQ
// sources recorded at registration and refreshed on connect; [517..532]
// per-source dispatch deltas; [533..540] task priority; [541..548] READY-to-run
// sample count; [549..556] last latency in us; [557..564] max latency in us;
// [565..567] connection-parameter lookup/update rc and update status (-1 means
// update was disabled at compile time);
// [568..570] negotiated interval, latency, supervision timeout;
// [571] wake timestamp race count; [572] connection-period pend-switch delta.
// [573] cumulative BT controller IRQ dispatches (C5 source 4 / C6 BT_MAC or BT_BB);
// [574] cumulative LP timer IRQ dispatches (ETS_LP_TIMER_INTR_SOURCE), counted
// from boot including advertising.
// [575] default 1M PHY preference rc; [576] minimum data-length request rc;
// [577] security profile (1=Just Works, no bond); [578] boost apply count;
// [579] current boost state; [580] max NimBLE-host READY-to-run latency during
// the current BLE connection; [581] event sequence; [582] valid event count;
// [583..798] 24 records of nine words each:
// {sequence, elapsed-us-since-connect, GAP-event, conn-handle, status,
// detail0..detail3}. After 24 events, each new event overwrites the oldest
// slot. To read oldest first, begin at slot (sequence-count) % 24 and wrap.
// Synthetic event 0x10000 records boost transitions (status 0=applied,
// status -1=compat update failed; detail0=enabled, detail1=2M, detail2=DLE251).
// [799..802] max source-4 dispatch gap, elapsed time at its end, and prior
// interrupted mepc/task; [803..810] gap histogram (<1,<5,<15,<35,<70,<200,
// <720,>=720 ms); [811..812] large-gap ring next slot/count; [813..832] four
// records {elapsed-us,gap-us,prior mepc,prior task,end kind}; end kind 0=next
// source-4 dispatch, 1=disconnect callback; [833] disconnect elapsed-us;
// [834..838] max masked duration by kind; [839..843] matching caller PCs;
// [844..848] matching compat task IDs; [849..853] matching sample counts.
// Mask kinds: 0 compat port/critical, 1 esp_intr allocator lock,
// 2 ESP IRQ registration, 3 FreeRTOS ISR mask API, 4 BT handler while
// trap MIE is clear. [854] BT_LE_SLEEP_ENABLE; [855] BT_LE_LP_CLK_SRC
// (0=RTC slow, 1=main XTAL, 2=other/unknown); [856] RTC slow-clock source
// (0=external 32K, 1=136K RC, 2=other/unknown); [857] controller SCA ppm;
// [858] PM_ENABLE; [859] FREERTOS_USE_TICKLESS_IDLE;
// [860] BT_NIMBLE_USE_ESP_TIMER (NimBLE NPL callout backend);
// [861] PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP; [862] PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP.
// [855] is the BT-specific Kconfig source selection; [856] is the system RTC slow clock,
// consulted by IDF only when BT_LE_LP_CLK_SRC_MAIN_XTAL is not selected.
// [863..864] RSSI query count/success count; [865..866] min/max valid dBm;
// [867..868] last dBm/rc; [869..871] last valid dBm/rc/query-start elapsed-us
// copied at disconnect; subtract [871] from [833] for the query age at disconnect;
// [872] ring next slot; [873..896] eight records {query-start elapsed-us,signed dBm,rc};
// [897..898] RSSI callout init/reset rc; [899] CONFIG_XTAL_FREQ in MHz;
// [900] IDF controller default BT LP-clock frequency (100000 Hz);
// [901] PHY cal-data storage; [902] configured PHY calibration mode;
// [903] throughput stream enabled; [904] successful notifications;
// [905] notify/allocation errors; [906] most recent notify rc;
// [907] current ATT MTU; [908] most recent notification value length;
// [909] throughput callout init rc.
// Details: MTU={CID,MTU}; CONN_UPDATE={interval,latency,timeout}; update request
// ={interval_min,interval_max,latency,timeout}; PHY={TX,RX}; DATA_LEN={TX octets,
// TX time,RX octets,RX time}; PASSKEY={action}; DISCONNECT={interval,latency,
// timeout}; REPEAT_PAIRING={response,current key size,new key size,new bonding}.
// Task slots match task_diag order; interval units are 1.25ms and supervision
// timeout units are 10ms.
// Values 0x7fffffff mean the corresponding call has not returned.
#if defined(__riscv)
__attribute__((used, aligned(4), section(".noinit")))
#else
__attribute__((used, aligned(4)))
#endif
volatile shizuku_esp_ble_diag_t shizuku_esp_ble_diag;
// Per-object live state, indexed by compat object id: kind, creator PC, owner,
// recursion, count, waiter count, last taker, last giver, take/give totals,
// highest-priority waiter, owner task state. This remains separate to keep the
// stable main diag compact.
volatile uint32_t shizuku_esp_ble_sync_object_diag[24][14];
// address/value/kind/caller-PC for the most recent esp_libc lock acquire.
volatile uint32_t shizuku_esp_ble_libc_lock_diag[4];
// last set state, last getter return, getter count, slot of last query,
// scheduler state immediately before/after CORE slot 20.
volatile uint32_t shizuku_esp_ble_scheduler_diag[6];
volatile uint32_t shizuku_esp_ble_timeout_shim_calls = 0;
volatile uint32_t shizuku_esp_ble_queue_sem_shim_calls = 0;
volatile uint32_t shizuku_esp_ble_critical_shim_calls = 0;
extern struct npl_funcs_t *r_npl_funcs;
}

namespace {
#if defined(SHIZUKU_ESP32_C6)
constexpr char kBleDeviceName[] = "Shizuku-C6";
#else
constexpr char kBleDeviceName[] = "Shizuku-C5";
#endif
constexpr uint16_t kBleAppearance = 0x0080; // Generic Computer
constexpr uint16_t kShizukuGattServiceUuid = 0xFFF0;
constexpr uint16_t kShizukuGattBuildVersionUuid = 0xFFF1;
constexpr uint16_t kShizukuGattThroughputUuid = 0xFFF2;
constexpr size_t kMaxNotifyValueLength = 514; // ATT MTU 517 minus opcode and handle.
constexpr uint32_t kBleCompatGrantCycles = 1600u; // 100 us at the C5/C6 16 MHz timer rate.
// Regression probes are opt-in so the stage-8 baseline has no connection-only
// update request or pend-switch sampling overhead.
#ifndef SHIZUKU_ESP_BLE_ENABLE_CONN_PARAM_UPDATE
#define SHIZUKU_ESP_BLE_ENABLE_CONN_PARAM_UPDATE 0
#endif
#ifndef SHIZUKU_ESP_BLE_ENABLE_PEND_SWITCH_DIAG
#define SHIZUKU_ESP_BLE_ENABLE_PEND_SWITCH_DIAG 0
#endif
// Recovery baseline is the a2b175c profile: leave these probes off. The
// 1M API sets a preference only; PHY capability removal is an IDF Kconfig
// choice (BT_NIMBLE_50_FEATURE_SUPPORT and its 2M/Coded PHY options).
// Just Works additionally requires CONFIG_BT_NIMBLE_SECURITY_ENABLE=y.
#ifndef SHIZUKU_ESP_BLE_PREFER_1M_PHY
#define SHIZUKU_ESP_BLE_PREFER_1M_PHY 0
#endif
#ifndef SHIZUKU_ESP_BLE_LIMIT_DLE
#define SHIZUKU_ESP_BLE_LIMIT_DLE 0
#endif
#ifndef SHIZUKU_ESP_BLE_ENABLE_SCHED_BOOST
#define SHIZUKU_ESP_BLE_ENABLE_SCHED_BOOST 1
#endif
#ifndef SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
#define SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG 1
#endif
#ifndef SHIZUKU_ESP_BLE_JUST_WORKS_NO_BOND
#define SHIZUKU_ESP_BLE_JUST_WORKS_NO_BOND 0
#endif
constexpr size_t kBleEventCapacity = 24;
constexpr uint32_t kBleBoostPriority = 8u;
constexpr uint32_t kBleBoostIdlePollUs = 25u;
constexpr uint32_t kBleSyntheticBoostEvent = 0x10000u;
constexpr uint32_t kBleLargeIrqGapUs = 15000u;
static ble_uuid16_t g_service_uuid{};
static ble_uuid16_t g_build_version_uuid{};
static ble_uuid16_t g_throughput_uuid{};
static ble_gatt_chr_def g_gatt_characteristics[3]{};
static ble_gatt_svc_def g_gatt_services[2]{};
static uint16_t g_build_version_value_handle = 0;
static uint16_t g_throughput_value_handle = 0;
static struct ble_npl_callout g_adv_retry_callout{};
static bool g_adv_retry_callout_ready = false;
static struct ble_npl_callout g_rssi_callout{};
static bool g_rssi_callout_ready = false;
static struct ble_npl_callout g_throughput_callout{};
static bool g_throughput_callout_ready = false;
static bool g_throughput_enabled = false;
static bool g_throughput_subscribed = false;
static uint32_t g_throughput_sequence = 0;
static uint16_t g_throughput_payload_len = 0;
static shizuku::esp_idf_support::ble_chip::irq_dispatch_snapshot g_ble_conn_irq_baseline[16]{};
static size_t g_ble_conn_irq_baseline_count = 0;
static uint32_t g_ble_conn_started_us = 0;
static uint32_t g_ble_conn_pend_switch_baseline = 0;
static uint32_t g_ble_event_epoch_us = 0;
static bool g_ble_connection_active = false;
static bool g_ble_phy_2m = false;
static bool g_ble_dle_251 = false;
static bool g_ble_irq_gap_baseline_valid = false;
static uint32_t g_ble_irq_last_dispatch_us = 0;
static uint32_t g_ble_irq_last_mepc = 0;
static uint32_t g_ble_irq_last_task_id = 0;
struct irq_mask_sample { uint32_t kind, started_us, caller_pc, task_id; };
static irq_mask_sample g_ble_irq_mask_stack[8]{};
static uint32_t g_ble_irq_mask_depth = 0;
static struct ble_npl_event *(*g_npl_eventq_get_original)(
    struct ble_npl_eventq *, ble_npl_time_t) = nullptr;

bool shizuku_esp_ble_is_internal_ram(uintptr_t address) {
    return (address >= 0x40800000u && address < 0x40860000u) ||
           (address >= 0x50000000u && address < 0x50004000u);
}

extern "C" struct ble_npl_event *shizuku_esp_ble_probe_eventq_get(
    struct ble_npl_eventq *queue, ble_npl_time_t timeout) {
    auto &diag = shizuku_esp_ble_diag;
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
    if (queue && shizuku_esp_ble_is_internal_ram(reinterpret_cast<uintptr_t>(queue))) {
        auto *storage = static_cast<volatile uint32_t *>(queue->eventq);
        const uintptr_t storage_address = reinterpret_cast<uintptr_t>(storage);
        if (storage && shizuku_esp_ble_is_internal_ram(storage_address)) {
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
    if (event && shizuku_esp_ble_is_internal_ram(reinterpret_cast<uintptr_t>(event)))
    {
        const uint32_t event_payload =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event->event));
        diag.npl_eventq_probe_return_event = event_payload;
        record[7] = event_payload;
    }
    asm volatile("fence rw,rw" ::: "memory");
    return event;
}

void shizuku_esp_ble_install_npl_probe() {
    auto *host = npl_funcs;
    auto *controller = r_npl_funcs;
    shizuku_esp_ble_diag.npl_host_funcs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host));
    shizuku_esp_ble_diag.npl_controller_funcs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller));
    if (host) shizuku_esp_ble_diag.npl_host_eventq_get =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host->p_ble_npl_eventq_get));
    if (controller) {
        shizuku_esp_ble_diag.npl_controller_eventq_get =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller->p_ble_npl_eventq_get));
        shizuku_esp_ble_diag.npl_controller_eventq_is_empty =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(controller->p_ble_npl_eventq_is_empty));
        if (controller->p_ble_npl_eventq_get) {
            g_npl_eventq_get_original = controller->p_ble_npl_eventq_get;
            controller->p_ble_npl_eventq_get = shizuku_esp_ble_probe_eventq_get;
            shizuku_esp_ble_diag.npl_eventq_probe_function =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                    shizuku_esp_ble_probe_eventq_get));
        }
    }
    asm volatile("fence rw,rw" ::: "memory");
}

int shizuku_esp_ble_gap_event(struct ble_gap_event *event, void *arg);
int shizuku_esp_ble_start_advertising(void);
void shizuku_esp_ble_schedule_adv_retry(uint32_t delay_ms);

void shizuku_esp_ble_record_gap_event(uint32_t type, uint16_t conn_handle,
                                     int32_t status, uint32_t detail0 = 0,
                                     uint32_t detail1 = 0, uint32_t detail2 = 0,
                                     uint32_t detail3 = 0) {
    uint32_t now_us = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us());
    if (type == BLE_GAP_EVENT_CONNECT) g_ble_event_epoch_us = now_us;
    const uint32_t sequence = shizuku_esp_ble_diag.ble_event_sequence + 1u;
    const size_t slot = (sequence - 1u) % kBleEventCapacity;
    volatile uint32_t *record = shizuku_esp_ble_diag.ble_event_records[slot];
    record[0] = sequence;
    record[1] = now_us - g_ble_event_epoch_us;
    record[2] = type;
    record[3] = conn_handle;
    record[4] = static_cast<uint32_t>(status);
    record[5] = detail0;
    record[6] = detail1;
    record[7] = detail2;
    record[8] = detail3;
    shizuku_esp_ble_diag.ble_event_sequence = sequence;
    if (shizuku_esp_ble_diag.ble_event_count < kBleEventCapacity)
        shizuku_esp_ble_diag.ble_event_count += 1u;
}
void shizuku_esp_ble_record_rssi(uint16_t conn_handle);
void shizuku_esp_ble_rssi_callout_event(struct ble_npl_event *event);

void shizuku_esp_ble_update_scheduler_boost(uint16_t conn_handle) {
    const bool should_boost = SHIZUKU_ESP_BLE_ENABLE_SCHED_BOOST &&
        g_ble_connection_active && (g_ble_phy_2m || g_ble_dle_251);
    const bool was_boosted = shizuku_esp_ble_diag.ble_scheduler_boost_active != 0;
    if (should_boost == was_boosted) return;
    const uint32_t task_id = shizuku_esp_ble_diag.nimble_host_task_id;
    const BaseType_t rc = task_id
        ? shizuku_freertos_compat_set_ble_scheduler_boost(
              task_id, should_boost ? 1u : 0u, kBleBoostPriority,
              kBleBoostIdlePollUs)
        : pdFAIL;
    if (rc != pdPASS) {
        shizuku_esp_ble_record_gap_event(kBleSyntheticBoostEvent, conn_handle,
                                        -1, should_boost ? 1u : 0u);
        return;
    }
    shizuku_esp_ble_diag.ble_scheduler_boost_active = should_boost ? 1u : 0u;
    if (should_boost)
        shizuku_esp_ble_diag.ble_scheduler_boost_apply_count += 1u;
    shizuku_esp_ble_record_gap_event(kBleSyntheticBoostEvent, conn_handle, 0,
                                    should_boost ? 1u : 0u,
                                    g_ble_phy_2m ? 1u : 0u,
                                    g_ble_dle_251 ? 1u : 0u);
}

void shizuku_esp_ble_irq_mask_begin_impl(uint32_t kind, uint32_t caller_pc,
                                        uint32_t task_id) {
#if SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
    if (!g_ble_connection_active) return;
    const uint32_t depth = g_ble_irq_mask_depth;
    if (depth >= sizeof(g_ble_irq_mask_stack) / sizeof(g_ble_irq_mask_stack[0]))
        return;
    g_ble_irq_mask_stack[depth] = {kind,
        static_cast<uint32_t>(shizuku::esp_idf_support::ble_chip::time_us()), caller_pc, task_id};
    g_ble_irq_mask_depth = depth + 1u;
#else
    (void)kind; (void)caller_pc; (void)task_id;
#endif
}

void shizuku_esp_ble_irq_mask_end_impl(uint32_t kind) {
#if SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
    const uint32_t depth = g_ble_irq_mask_depth;
    if (!depth) return;
    const irq_mask_sample sample = g_ble_irq_mask_stack[depth - 1u];
    if (sample.kind != kind) return;
    g_ble_irq_mask_depth = depth - 1u;
    const uint32_t elapsed = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us()) - sample.started_us;
    if (kind >= 5u) return;
    shizuku_esp_ble_diag.ble_irq_mask_count[kind] += 1u;
    if (elapsed > shizuku_esp_ble_diag.ble_irq_mask_max_us[kind]) {
        shizuku_esp_ble_diag.ble_irq_mask_max_us[kind] = elapsed;
        shizuku_esp_ble_diag.ble_irq_mask_max_pc[kind] = sample.caller_pc;
        shizuku_esp_ble_diag.ble_irq_mask_max_task[kind] = sample.task_id;
    }
#else
    (void)kind;
#endif
}

void shizuku_esp_ble_record_bt_irq_gap(uint32_t end_us, uint32_t end_kind) {
#if SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
    if (!g_ble_connection_active || !g_ble_irq_gap_baseline_valid) return;
    const uint32_t gap_us = end_us - g_ble_irq_last_dispatch_us;
    const uint32_t elapsed_us = end_us - g_ble_conn_started_us;
    const uint32_t bucket = gap_us < 1000u ? 0u : gap_us < 5000u ? 1u :
        gap_us < 15000u ? 2u : gap_us < 35000u ? 3u :
        gap_us < 70000u ? 4u : gap_us < 200000u ? 5u :
        gap_us < 720000u ? 6u : 7u;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_histogram[bucket] += 1u;
    if (gap_us > shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_us) {
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_us = gap_us;
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_elapsed_us = elapsed_us;
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_prior_mepc = g_ble_irq_last_mepc;
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_prior_task = g_ble_irq_last_task_id;
    }
    if (gap_us >= kBleLargeIrqGapUs) {
        const uint32_t slot = shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_head % 4u;
        volatile uint32_t *record = shizuku_esp_ble_diag.ble_conn_bt_irq_gap_records[slot];
        record[0] = elapsed_us;
        record[1] = gap_us;
        record[2] = g_ble_irq_last_mepc;
        record[3] = g_ble_irq_last_task_id;
        record[4] = end_kind;
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_head = (slot + 1u) % 4u;
        if (shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_count < 4u)
            shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_count += 1u;
    }
#else
    (void)end_us; (void)end_kind;
#endif
    g_ble_irq_last_dispatch_us = end_us;
}

void shizuku_esp_ble_note_irq_dispatch_impl(uint32_t source,
                                           uint32_t interrupted_mepc,
                                           uint32_t task_id) {
#if defined(SHIZUKU_ESP32_C6)
    const bool is_bt_irq = source == ETS_BT_MAC_INTR_SOURCE ||
                           source == ETS_BT_BB_INTR_SOURCE;
#else
    // C5's validated controller image routes its BT service through source 4.
    const bool is_bt_irq = source == 4u;
#endif
    if (source == ETS_LP_TIMER_INTR_SOURCE) {
        shizuku_esp_ble_diag.ble_irq_source7_dispatch_count += 1u;
        return;
    }
    if (!is_bt_irq) return;
    shizuku_esp_ble_diag.ble_irq_source4_dispatch_count += 1u;
#if SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
    if (!g_ble_connection_active) return;
    const uint32_t now_us = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us());
    if (g_ble_irq_gap_baseline_valid)
        shizuku_esp_ble_record_bt_irq_gap(now_us, 0u);
    else
        g_ble_irq_gap_baseline_valid = true;
    g_ble_irq_last_dispatch_us = now_us;
    g_ble_irq_last_mepc = interrupted_mepc;
    g_ble_irq_last_task_id = task_id;
#else
    (void)interrupted_mepc; (void)task_id;
#endif
}

void shizuku_esp_ble_capture_connection_irq_baseline() {
    g_ble_irq_gap_baseline_valid = false;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_us = 0;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_elapsed_us = 0;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_prior_mepc = 0;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_max_prior_task = 0;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_head = 0;
    shizuku_esp_ble_diag.ble_conn_bt_irq_gap_ring_count = 0;
    shizuku_esp_ble_diag.ble_conn_disconnect_elapsed_us = 0;
    for (size_t i = 0; i < 5; ++i) {
        shizuku_esp_ble_diag.ble_irq_mask_max_us[i] = 0;
        shizuku_esp_ble_diag.ble_irq_mask_max_pc[i] = 0;
        shizuku_esp_ble_diag.ble_irq_mask_max_task[i] = 0;
        shizuku_esp_ble_diag.ble_irq_mask_count[i] = 0;
    }
    for (size_t i = 0; i < 8; ++i)
        shizuku_esp_ble_diag.ble_conn_bt_irq_gap_histogram[i] = 0;
    for (size_t i = 0; i < 4; ++i)
        for (size_t j = 0; j < 5; ++j)
            shizuku_esp_ble_diag.ble_conn_bt_irq_gap_records[i][j] = 0;
    g_ble_conn_started_us = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us());
#if SHIZUKU_ESP_BLE_ENABLE_PEND_SWITCH_DIAG
    g_ble_conn_pend_switch_baseline = shizuku::esp_idf_support::ble_chip::pend_switch_count();
#else
    g_ble_conn_pend_switch_baseline = 0;
#endif
    g_ble_conn_irq_baseline_count = shizuku::esp_idf_support::ble_chip::irq_snapshot(
        g_ble_conn_irq_baseline, sizeof(g_ble_conn_irq_baseline) /
                                     sizeof(g_ble_conn_irq_baseline[0]));
    if (g_ble_conn_irq_baseline_count >
        sizeof(g_ble_conn_irq_baseline) / sizeof(g_ble_conn_irq_baseline[0]))
        g_ble_conn_irq_baseline_count =
            sizeof(g_ble_conn_irq_baseline) / sizeof(g_ble_conn_irq_baseline[0]);
    shizuku_esp_ble_diag.ble_conn_duration_us = 0;
    shizuku_esp_ble_diag.ble_conn_irq_registration_count =
        static_cast<uint32_t>(g_ble_conn_irq_baseline_count);
    shizuku_esp_ble_diag.ble_conn_irq_total_delta = 0;
    shizuku_esp_ble_diag.ble_conn_pend_switch_delta = 0;
    for (size_t i = 0; i < 16; ++i) {
        shizuku_esp_ble_diag.ble_conn_irq_sources[i] =
            i < g_ble_conn_irq_baseline_count ? g_ble_conn_irq_baseline[i].source : UINT32_MAX;
        shizuku_esp_ble_diag.ble_conn_irq_dispatch_deltas[i] = 0;
    }
}

void shizuku_esp_ble_capture_connection_irq_delta() {
    shizuku::esp_idf_support::ble_chip::irq_dispatch_snapshot current[16]{};
    const size_t reported_count = shizuku::esp_idf_support::ble_chip::irq_snapshot(
        current, sizeof(current) / sizeof(current[0]));
    const size_t current_count = reported_count < 16 ? reported_count : 16;
    shizuku_esp_ble_diag.ble_conn_duration_us = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us()) - g_ble_conn_started_us;
#if SHIZUKU_ESP_BLE_ENABLE_PEND_SWITCH_DIAG
    shizuku_esp_ble_diag.ble_conn_pend_switch_delta =
        shizuku::esp_idf_support::ble_chip::pend_switch_count() - g_ble_conn_pend_switch_baseline;
#else
    shizuku_esp_ble_diag.ble_conn_pend_switch_delta = 0;
#endif
    shizuku_esp_ble_diag.ble_conn_irq_total_delta = 0;
    for (size_t i = 0; i < g_ble_conn_irq_baseline_count; ++i) {
        uint32_t delta = UINT32_MAX;
        for (size_t j = 0; j < current_count; ++j) {
            if (current[j].source != g_ble_conn_irq_baseline[i].source) continue;
            delta = current[j].dispatch_count -
                    g_ble_conn_irq_baseline[i].dispatch_count;
            break;
        }
        shizuku_esp_ble_diag.ble_conn_irq_dispatch_deltas[i] = delta;
        if (delta != UINT32_MAX)
            shizuku_esp_ble_diag.ble_conn_irq_total_delta += delta;
    }
}

void shizuku_esp_ble_record_conn_params(const struct ble_gap_conn_desc &desc) {
    shizuku_esp_ble_diag.ble_conn_interval = desc.conn_itvl;
    shizuku_esp_ble_diag.ble_conn_latency = desc.conn_latency;
    shizuku_esp_ble_diag.ble_conn_supervision_timeout = desc.supervision_timeout;
}

int shizuku_esp_ble_read_conn_params(uint16_t conn_handle) {
    struct ble_gap_conn_desc desc{};
    const int rc = ble_gap_conn_find(conn_handle, &desc);
    shizuku_esp_ble_diag.ble_conn_params_find_rc = rc;
    if (rc == 0) shizuku_esp_ble_record_conn_params(desc);
    return rc;
}

void shizuku_esp_ble_adv_retry_event(struct ble_npl_event *) {
    const int rc = shizuku_esp_ble_start_advertising();
    shizuku_esp_ble_diag.adv_restart_count =
        shizuku_esp_ble_diag.adv_restart_count + 1u;
    shizuku_esp_ble_diag.adv_restart_last_rc = rc;
    if (rc != 0) shizuku_esp_ble_schedule_adv_retry(250);
}

void shizuku_esp_ble_set_stage(uint32_t stage) {
    if (shizuku_esp_ble_diag.stage < 0x30 &&
        shizuku_esp_ble_diag.stage < stage)
        shizuku_esp_ble_diag.stage = stage;
    asm volatile("fence rw,rw" ::: "memory");
}

[[noreturn]] void shizuku_esp_ble_cooperative_wait(void) {
    for (;;) {
        // The root Shizuku thread owns no implicit FreeRTOS time slice. Run
        // compat tasks explicitly; this grants each READY task for a bounded
        // 100 us quantum and lets blocked tasks wake through SLEEP_US/timer IRQs.
        shizuku_freertos_compat_run_scheduler(kBleCompatGrantCycles);
        // If the compat domain was never bound, keep the boot-failure path
        // cooperative too. With a bound domain, run_scheduler does not return.
        (void)shizuku::freertos_compat::shizuku_port::call(
            shizuku::object_api::SLEEP_US, 1000u);
    }
}

int shizuku_esp_ble_gatt_access(uint16_t, uint16_t,
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
    shizuku_esp_ble_diag.gatt_read_count =
        shizuku_esp_ble_diag.gatt_read_count + 1u;
    shizuku_esp_ble_set_stage(10);
    return 0;
}

int shizuku_esp_ble_throughput_access(uint16_t, uint16_t,
                                     struct ble_gatt_access_ctxt *ctxt, void *) {
    if (!ctxt) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t command = 0xff;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1 ||
            os_mbuf_copydata(ctxt->om, 0, sizeof(command), &command) != 0 ||
            command > 1u)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        g_throughput_enabled = command == 1u;
        shizuku_esp_ble_diag.ble_tx_enabled = g_throughput_enabled ? 1u : 0u;
        if (g_throughput_enabled && g_throughput_callout_ready) {
            ble_npl_time_t delay = ble_npl_time_ms_to_ticks32(1u);
            if (delay == 0) delay = 1;
            shizuku_esp_ble_diag.ble_tx_last_notify_rc =
                ble_npl_callout_reset(&g_throughput_callout, delay);
        } else if (!g_throughput_enabled && g_throughput_callout_ready) {
            (void)ble_npl_callout_stop(&g_throughput_callout);
        }
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || !g_throughput_enabled ||
        g_throughput_payload_len < 4u ||
        g_throughput_payload_len > kMaxNotifyValueLength)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t payload[kMaxNotifyValueLength];
    const uint32_t sequence = g_throughput_sequence;
    payload[0] = static_cast<uint8_t>(sequence);
    payload[1] = static_cast<uint8_t>(sequence >> 8);
    payload[2] = static_cast<uint8_t>(sequence >> 16);
    payload[3] = static_cast<uint8_t>(sequence >> 24);
    for (size_t i = 4; i < g_throughput_payload_len; ++i)
        payload[i] = static_cast<uint8_t>((sequence + i) & 0xffu);
    if (os_mbuf_append(ctxt->om, payload, g_throughput_payload_len) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

void shizuku_esp_ble_throughput_callout_event(struct ble_npl_event *) {
    if (!g_throughput_enabled || !g_throughput_subscribed ||
        !g_ble_connection_active || !g_throughput_callout_ready)
        return;
    const uint32_t raw_handle = shizuku_esp_ble_diag.ble_connection_handle;
    if (raw_handle > UINT16_MAX || g_throughput_value_handle == 0) return;

    const uint16_t conn_handle = static_cast<uint16_t>(raw_handle);
    const uint16_t mtu = ble_att_mtu(conn_handle);
    const uint16_t value_len = mtu > 3u
        ? static_cast<uint16_t>(MIN(mtu - 3u, kMaxNotifyValueLength)) : 0u;
    shizuku_esp_ble_diag.ble_tx_last_mtu = mtu;
    shizuku_esp_ble_diag.ble_tx_last_payload_len = value_len;
    int rc = value_len >= 4u ? 0 : BLE_HS_EMSGSIZE;
    if (rc == 0) {
        g_throughput_payload_len = value_len;
        // A NULL mbuf asks NimBLE to read the characteristic value through its
        // access callback, which emits the sequence and MTU-sized payload.
        rc = ble_gatts_notify_custom(conn_handle, g_throughput_value_handle, nullptr);
    }
    shizuku_esp_ble_diag.ble_tx_last_notify_rc = rc;
    if (rc == 0) {
        g_throughput_sequence += 1u;
        shizuku_esp_ble_diag.ble_tx_notifications += 1u;
    } else {
        shizuku_esp_ble_diag.ble_tx_errors += 1u;
    }

    if (g_throughput_enabled && g_throughput_subscribed && g_ble_connection_active) {
        ble_npl_time_t delay = ble_npl_time_ms_to_ticks32(1u);
        if (delay == 0) delay = 1;
        (void)ble_npl_callout_reset(&g_throughput_callout, delay);
    }
}

int shizuku_esp_ble_register_gatt(void) {
    g_service_uuid.u.type = BLE_UUID_TYPE_16;
    g_service_uuid.value = kShizukuGattServiceUuid;
    g_build_version_uuid.u.type = BLE_UUID_TYPE_16;
    g_build_version_uuid.value = kShizukuGattBuildVersionUuid;
    g_throughput_uuid.u.type = BLE_UUID_TYPE_16;
    g_throughput_uuid.value = kShizukuGattThroughputUuid;

    g_gatt_characteristics[0].uuid = &g_build_version_uuid.u;
    g_gatt_characteristics[0].access_cb = shizuku_esp_ble_gatt_access;
    g_gatt_characteristics[0].flags = BLE_GATT_CHR_F_READ;
    g_gatt_characteristics[0].val_handle = &g_build_version_value_handle;
    g_gatt_characteristics[1].uuid = &g_throughput_uuid.u;
    g_gatt_characteristics[1].access_cb = shizuku_esp_ble_throughput_access;
    g_gatt_characteristics[1].flags = BLE_GATT_CHR_F_READ |
        BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
    g_gatt_characteristics[1].val_handle = &g_throughput_value_handle;
    g_gatt_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_gatt_services[0].uuid = &g_service_uuid.u;
    g_gatt_services[0].characteristics = g_gatt_characteristics;

    int rc = ble_gatts_count_cfg(g_gatt_services);
    shizuku_esp_ble_diag.gatt_count_cfg_rc = rc;
    if (rc != 0) return rc;
    rc = ble_gatts_add_svcs(g_gatt_services);
    shizuku_esp_ble_diag.gatt_add_svcs_rc = rc;
    return rc;
}

int shizuku_esp_ble_start_advertising(void) {
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    shizuku_esp_ble_diag.adv_addr_infer_rc = rc;
    if (rc != 0) {
        return rc;
    }

    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(kBleDeviceName));
    fields.name_len = sizeof(kBleDeviceName) - 1u;
    fields.name_is_complete = 1;
    fields.uuids16 = &g_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = kBleAppearance;
    fields.appearance_is_present = 1;
    rc = ble_gap_adv_set_fields(&fields);
    shizuku_esp_ble_diag.adv_set_fields_rc = rc;
    if (rc != 0) return rc;

    struct ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &params,
                           shizuku_esp_ble_gap_event, nullptr);
    shizuku_esp_ble_diag.adv_start_rc = rc;
    if (rc == 0) {
        shizuku_esp_ble_set_stage(8);
        shizuku::esp_idf_support::ble_chip::capture_radio_state(7u);
    }
    return rc;
}

void shizuku_esp_ble_record_rssi(uint16_t conn_handle) {
    const uint32_t sample_time_us = static_cast<uint32_t>(
        shizuku::esp_idf_support::ble_chip::time_us());
    int8_t rssi_dbm = 127; // NimBLE/HCI unavailable sentinel until a successful result.
    const int rc = ble_gap_conn_rssi(conn_handle, &rssi_dbm);
    // The HCI command-complete ack is signaled directly by the VHCI receive callback.
    // Discard a reply if the host handled disconnect while this call was waiting.
    if (!g_ble_connection_active) return;
    shizuku_esp_ble_diag.ble_rssi_sample_count += 1u;
    shizuku_esp_ble_diag.ble_rssi_last_rc = rc;
    shizuku_esp_ble_diag.ble_rssi_last_dbm = (rc == 0) ? rssi_dbm : 127;

    if (rc == 0 && rssi_dbm != 127) {
        const int32_t signed_rssi = rssi_dbm;
        if (shizuku_esp_ble_diag.ble_rssi_valid_count == 0) {
            shizuku_esp_ble_diag.ble_rssi_min_dbm = signed_rssi;
            shizuku_esp_ble_diag.ble_rssi_max_dbm = signed_rssi;
        } else {
            const int32_t old_min = shizuku_esp_ble_diag.ble_rssi_min_dbm;
            const int32_t old_max = shizuku_esp_ble_diag.ble_rssi_max_dbm;
            if (signed_rssi < old_min)
                shizuku_esp_ble_diag.ble_rssi_min_dbm = signed_rssi;
            if (signed_rssi > old_max)
                shizuku_esp_ble_diag.ble_rssi_max_dbm = signed_rssi;
        }
        shizuku_esp_ble_diag.ble_rssi_valid_count += 1u;
    }

    const uint32_t slot = shizuku_esp_ble_diag.ble_rssi_ring_head % 8u;
    volatile uint32_t *record = shizuku_esp_ble_diag.ble_rssi_records[slot];
    record[0] = sample_time_us - g_ble_conn_started_us;
    record[1] = static_cast<uint32_t>(shizuku_esp_ble_diag.ble_rssi_last_dbm);
    record[2] = static_cast<uint32_t>(rc);
    shizuku_esp_ble_diag.ble_rssi_ring_head = (slot + 1u) % 8u;
    if (g_ble_connection_active && g_rssi_callout_ready) {
        ble_npl_time_t delay = ble_npl_time_ms_to_ticks32(1000u);
        if (delay == 0) delay = 1;
        shizuku_esp_ble_diag.ble_rssi_callout_reset_rc =
            ble_npl_callout_reset(&g_rssi_callout, delay);
    } else {
        shizuku_esp_ble_diag.ble_rssi_callout_reset_rc = -1;
    }
}

void shizuku_esp_ble_rssi_callout_event(struct ble_npl_event *) {
    if (!g_ble_connection_active) return;
    const uint32_t raw_handle = shizuku_esp_ble_diag.ble_connection_handle;
    if (raw_handle > UINT16_MAX) return;
    shizuku_esp_ble_record_rssi(static_cast<uint16_t>(raw_handle));
}

void shizuku_esp_ble_capture_disconnect_rssi(uint32_t disconnect_elapsed_us) {
    shizuku_esp_ble_diag.ble_rssi_disconnect_dbm = 127;
    shizuku_esp_ble_diag.ble_rssi_disconnect_rc = -1;
    shizuku_esp_ble_diag.ble_rssi_disconnect_elapsed_us = UINT32_MAX;
    for (uint32_t n = 0; n < 8u; ++n) {
        const uint32_t slot = (shizuku_esp_ble_diag.ble_rssi_ring_head + 7u - n) % 8u;
        volatile uint32_t *record = shizuku_esp_ble_diag.ble_rssi_records[slot];
        const int32_t rssi = static_cast<int32_t>(record[1]);
        if (static_cast<int32_t>(record[2]) != 0 || rssi == 127 ||
            record[0] > disconnect_elapsed_us) continue;
        shizuku_esp_ble_diag.ble_rssi_disconnect_dbm = rssi;
        shizuku_esp_ble_diag.ble_rssi_disconnect_rc = 0;
        shizuku_esp_ble_diag.ble_rssi_disconnect_elapsed_us = record[0];
        return;
    }
}

void shizuku_esp_ble_schedule_adv_retry(uint32_t delay_ms) {
    if (!g_adv_retry_callout_ready) return;
    ble_npl_time_t delay_ticks = ble_npl_time_ms_to_ticks32(delay_ms);
    if (delay_ticks == 0) delay_ticks = 1;
    shizuku_esp_ble_diag.adv_retry_schedule_rc =
        ble_npl_callout_reset(&g_adv_retry_callout, delay_ticks);
}

void shizuku_esp_ble_on_reset(int reason) {
    shizuku_esp_ble_diag.nimble_reset_count =
        shizuku_esp_ble_diag.nimble_reset_count + 1u;
    shizuku_esp_ble_diag.nimble_last_reset_reason = reason;
}

void shizuku_esp_ble_on_sync(void) {
    shizuku_esp_ble_diag.nimble_sync_count =
        shizuku_esp_ble_diag.nimble_sync_count + 1u;
    shizuku_esp_ble_set_stage(6);
    if (shizuku_esp_ble_diag.gatt_add_svcs_rc != 0) return;
    shizuku_esp_ble_set_stage(7);
#if SHIZUKU_ESP_BLE_PREFER_1M_PHY
    shizuku_esp_ble_diag.ble_default_phy_rc =
        ble_gap_set_prefered_default_le_phy(BLE_HCI_LE_PHY_1M_PREF_MASK,
                                            BLE_HCI_LE_PHY_1M_PREF_MASK);
#else
    shizuku_esp_ble_diag.ble_default_phy_rc = -1;
#endif
    if (!g_adv_retry_callout_ready) {
        shizuku_esp_ble_diag.adv_callout_init_rc = ble_npl_callout_init(
            &g_adv_retry_callout, nimble_port_get_dflt_eventq(),
            shizuku_esp_ble_adv_retry_event, nullptr);
        g_adv_retry_callout_ready = shizuku_esp_ble_diag.adv_callout_init_rc == 0;
    }
    if (!g_rssi_callout_ready) {
        shizuku_esp_ble_diag.ble_rssi_callout_init_rc = ble_npl_callout_init(
            &g_rssi_callout, nimble_port_get_dflt_eventq(),
            shizuku_esp_ble_rssi_callout_event, nullptr);
        g_rssi_callout_ready = shizuku_esp_ble_diag.ble_rssi_callout_init_rc == 0;
    }
    if (!g_throughput_callout_ready) {
        shizuku_esp_ble_diag.ble_tx_callout_init_rc = ble_npl_callout_init(
            &g_throughput_callout, nimble_port_get_dflt_eventq(),
            shizuku_esp_ble_throughput_callout_event, nullptr);
        g_throughput_callout_ready = shizuku_esp_ble_diag.ble_tx_callout_init_rc == 0;
    }
    if (shizuku_esp_ble_start_advertising() != 0) {
        shizuku_esp_ble_diag.stage = 0x38;
        shizuku_esp_ble_schedule_adv_retry(250);
        asm volatile("fence rw,rw" ::: "memory");
    }
}

int shizuku_esp_ble_gap_event(struct ble_gap_event *event, void *) {
    if (!event) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_CONNECT,
            event->connect.conn_handle, event->connect.status);
        if (event->connect.status == 0) {
            g_ble_irq_gap_baseline_valid = false;
            g_ble_connection_active = true;
            g_throughput_enabled = false;
            g_throughput_subscribed = false;
            shizuku_esp_ble_diag.ble_tx_enabled = 0;
            g_ble_phy_2m = false;
            g_ble_dle_251 = false;
            shizuku_esp_ble_diag.ble_conn_nimble_host_max_wake_to_run_us = 0;
            shizuku_esp_ble_diag.ble_rssi_sample_count = 0;
            shizuku_esp_ble_diag.ble_rssi_valid_count = 0;
            shizuku_esp_ble_diag.ble_rssi_min_dbm = 127;
            shizuku_esp_ble_diag.ble_rssi_max_dbm = 127;
            shizuku_esp_ble_diag.ble_rssi_last_dbm = 127;
            shizuku_esp_ble_diag.ble_rssi_last_rc = 0x7fffffff;
            shizuku_esp_ble_diag.ble_rssi_disconnect_dbm = 127;
            shizuku_esp_ble_diag.ble_rssi_disconnect_rc = 0x7fffffff;
            shizuku_esp_ble_diag.ble_rssi_disconnect_elapsed_us = 0;
            shizuku_esp_ble_diag.ble_rssi_ring_head = 0;
            shizuku_esp_ble_diag.ble_rssi_callout_reset_rc = 0x7fffffff;
            for (size_t i = 0; i < 8; ++i)
                for (size_t j = 0; j < 3; ++j)
                    shizuku_esp_ble_diag.ble_rssi_records[i][j] =
                        j == 1u ? 127u : (j == 2u ? UINT32_MAX : 0u);
            shizuku_esp_ble_update_scheduler_boost(event->connect.conn_handle);
            shizuku_esp_ble_diag.ble_connect_count =
                shizuku_esp_ble_diag.ble_connect_count + 1u;
            shizuku_esp_ble_diag.ble_connection_handle = event->connect.conn_handle;
            shizuku_esp_ble_diag.ble_conn_params_find_rc = 0x7fffffff;
            shizuku_esp_ble_diag.ble_conn_params_update_rc = 0x7fffffff;
            shizuku_esp_ble_diag.ble_conn_params_update_status = 0x7fffffff;
            shizuku_esp_ble_diag.ble_conn_interval = UINT32_MAX;
            shizuku_esp_ble_diag.ble_conn_latency = UINT32_MAX;
            shizuku_esp_ble_diag.ble_conn_supervision_timeout = UINT32_MAX;
            shizuku_esp_ble_capture_connection_irq_baseline();
            (void)shizuku_esp_ble_read_conn_params(event->connect.conn_handle);
            shizuku_esp_ble_record_rssi(event->connect.conn_handle);
#if SHIZUKU_ESP_BLE_PREFER_1M_PHY
            // Apply the 1M preference to this connection as well as the
            // host default, including connections where the peer requests PHY.
            (void)ble_gap_set_prefered_le_phy(event->connect.conn_handle,
                BLE_HCI_LE_PHY_1M_PREF_MASK,
                BLE_HCI_LE_PHY_1M_PREF_MASK, 0);
#endif
#if SHIZUKU_ESP_BLE_LIMIT_DLE
            shizuku_esp_ble_diag.ble_conn_data_len_rc = ble_gap_set_data_len(
                event->connect.conn_handle,
                BLE_HCI_SET_DATALEN_TX_OCTETS_MIN,
                BLE_HCI_SET_DATALEN_TX_TIME_MIN);
#else
            shizuku_esp_ble_diag.ble_conn_data_len_rc = -1;
#endif
#if SHIZUKU_ESP_BLE_ENABLE_CONN_PARAM_UPDATE
            struct ble_gap_upd_params requested{};
            requested.itvl_min = 12; // 15 ms
            requested.itvl_max = 24; // 30 ms
            requested.latency = 0;
            requested.supervision_timeout = 400; // 4 s
            requested.min_ce_len = 0;
            requested.max_ce_len = 0;
            shizuku_esp_ble_diag.ble_conn_params_update_rc =
                ble_gap_update_params(event->connect.conn_handle, &requested);
#else
            shizuku_esp_ble_diag.ble_conn_params_update_rc = -1;
#endif
            shizuku_esp_ble_set_stage(9);
        } else {
            shizuku_esp_ble_diag.ble_connect_fail_status = event->connect.status;
            shizuku_esp_ble_schedule_adv_retry(100);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        {
        const uint32_t disconnect_time_us = static_cast<uint32_t>(
            shizuku::esp_idf_support::ble_chip::time_us());
        shizuku_esp_ble_record_bt_irq_gap(disconnect_time_us, 1u);
        shizuku_esp_ble_diag.ble_conn_disconnect_elapsed_us =
            disconnect_time_us - g_ble_conn_started_us;
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_DISCONNECT,
            event->disconnect.conn.conn_handle, event->disconnect.reason,
            event->disconnect.conn.conn_itvl,
            event->disconnect.conn.conn_latency,
            event->disconnect.conn.supervision_timeout);
        shizuku_esp_ble_diag.ble_disconnect_count =
            shizuku_esp_ble_diag.ble_disconnect_count + 1u;
        g_throughput_enabled = false;
        g_throughput_subscribed = false;
        shizuku_esp_ble_diag.ble_tx_enabled = 0;
        if (g_throughput_callout_ready)
            (void)ble_npl_callout_stop(&g_throughput_callout);
        shizuku_esp_ble_diag.ble_disconnect_reason = event->disconnect.reason;
        shizuku_esp_ble_diag.ble_connection_handle = UINT32_MAX;
        shizuku_esp_ble_record_conn_params(event->disconnect.conn);
        shizuku_esp_ble_capture_connection_irq_delta();
        g_ble_connection_active = false;
        shizuku_esp_ble_capture_disconnect_rssi(
            shizuku_esp_ble_diag.ble_conn_disconnect_elapsed_us);
        g_ble_phy_2m = false;
        g_ble_dle_251 = false;
        shizuku_esp_ble_update_scheduler_boost(event->disconnect.conn.conn_handle);
        shizuku_esp_ble_schedule_adv_retry(100);
        return 0;
        }
    case BLE_GAP_EVENT_CONN_UPDATE:
        shizuku_esp_ble_diag.ble_conn_params_update_status = event->conn_update.status;
        if (event->conn_update.status == 0) {
            (void)shizuku_esp_ble_read_conn_params(event->conn_update.conn_handle);
            shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_CONN_UPDATE,
                event->conn_update.conn_handle, event->conn_update.status,
                shizuku_esp_ble_diag.ble_conn_interval,
                shizuku_esp_ble_diag.ble_conn_latency,
                shizuku_esp_ble_diag.ble_conn_supervision_timeout);
        } else {
            shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_CONN_UPDATE,
                event->conn_update.conn_handle, event->conn_update.status);
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_throughput_value_handle) {
            g_throughput_subscribed = event->subscribe.cur_notify != 0;
            if (!g_throughput_subscribed) {
                g_throughput_enabled = false;
                shizuku_esp_ble_diag.ble_tx_enabled = 0;
                if (g_throughput_callout_ready)
                    (void)ble_npl_callout_stop(&g_throughput_callout);
            }
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        const auto *peer = event->conn_update_req.peer_params;
        shizuku_esp_ble_record_gap_event(event->type,
            event->conn_update_req.conn_handle, 0,
            peer ? peer->itvl_min : UINT32_MAX,
            peer ? peer->itvl_max : UINT32_MAX,
            peer ? peer->latency : UINT32_MAX,
            peer ? peer->supervision_timeout : UINT32_MAX);
        // Keep the peer's values in self_params (NimBLE initializes it as a
        // copy of peer_params) and accept; Shizuku does not initiate updates.
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_MTU,
            event->mtu.conn_handle, 0, event->mtu.channel_id, event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_PHY_UPDATE_COMPLETE,
            event->phy_updated.conn_handle, event->phy_updated.status,
            event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        if (event->phy_updated.status == 0 && g_ble_connection_active) {
            g_ble_phy_2m = event->phy_updated.tx_phy == 2u ||
                           event->phy_updated.rx_phy == 2u;
            shizuku_esp_ble_update_scheduler_boost(event->phy_updated.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_DATA_LEN_CHG,
            event->data_len_chg.conn_handle, 0,
            event->data_len_chg.max_tx_octets, event->data_len_chg.max_tx_time,
            event->data_len_chg.max_rx_octets, event->data_len_chg.max_rx_time);
        if (g_ble_connection_active) {
            g_ble_dle_251 = event->data_len_chg.max_tx_octets >= 251u ||
                            event->data_len_chg.max_rx_octets >= 251u;
            shizuku_esp_ble_update_scheduler_boost(event->data_len_chg.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_ENC_CHANGE,
            event->enc_change.conn_handle, event->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // Log only the action, never the passkey or numeric-comparison value.
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_PASSKEY_ACTION,
            event->passkey.conn_handle, 0, event->passkey.params.action);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
#if SHIZUKU_ESP_BLE_JUST_WORKS_NO_BOND
        struct ble_gap_conn_desc desc{};
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        const int response = rc == 0 ? BLE_GAP_REPEAT_PAIRING_RETRY
                                     : BLE_GAP_REPEAT_PAIRING_IGNORE;
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_REPEAT_PAIRING,
            event->repeat_pairing.conn_handle, rc, response,
            event->repeat_pairing.cur_key_size,
            event->repeat_pairing.new_key_size,
            event->repeat_pairing.new_bonding);
        return response;
#else
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_REPEAT_PAIRING,
            event->repeat_pairing.conn_handle, 0,
            BLE_GAP_REPEAT_PAIRING_IGNORE,
            event->repeat_pairing.cur_key_size,
            event->repeat_pairing.new_key_size,
            event->repeat_pairing.new_bonding);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
#endif
    }
    case BLE_GAP_EVENT_PARING_COMPLETE:
        shizuku_esp_ble_record_gap_event(BLE_GAP_EVENT_PARING_COMPLETE,
            event->pairing_complete.conn_handle, event->pairing_complete.status);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        shizuku_esp_ble_schedule_adv_retry(100);
        return 0;
    default:
        return 0;
    }
}

void shizuku_esp_ble_host_task(void *) {
    shizuku_esp_ble_diag.nimble_host_task_start_count =
        shizuku_esp_ble_diag.nimble_host_task_start_count + 1u;
    shizuku_esp_ble_diag.nimble_host_task_id = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()));
    nimble_port_run();
    nimble_port_freertos_deinit();
    shizuku_esp_ble_diag.nimble_host_returned = 1;
}
}

extern "C" {
void shizuku_esp_ble_irq_mask_begin(uint32_t kind, uint32_t caller_pc,
                                   uint32_t task_id) {
    shizuku_esp_ble_irq_mask_begin_impl(kind, caller_pc, task_id);
}
void shizuku_esp_ble_irq_mask_end(uint32_t kind) {
    shizuku_esp_ble_irq_mask_end_impl(kind);
}
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
        __atomic_fetch_add(&shizuku_esp_ble_diag.heap_init_malloc_calls, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&shizuku_esp_ble_diag.heap_init_malloc_requested,
                           static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        if (result) {
            __atomic_fetch_add(&shizuku_esp_ble_diag.heap_init_malloc_succeeded,
                               static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        }
    } else if (phase == heap_trace_phase::controller_init) {
        __atomic_fetch_add(&shizuku_esp_ble_diag.controller_malloc_calls, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&shizuku_esp_ble_diag.controller_malloc_requested,
                           static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        if (result) {
            __atomic_fetch_add(&shizuku_esp_ble_diag.controller_malloc_succeeded,
                               static_cast<uint32_t>(size), __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&shizuku_esp_ble_diag.controller_malloc_failures,
                               1u, __ATOMIC_RELAXED);
            uint32_t expected = 0;
            __atomic_compare_exchange_n(&shizuku_esp_ble_diag.controller_malloc_first_failed_size,
                                        &expected, static_cast<uint32_t>(size), false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            expected = 0;
            __atomic_compare_exchange_n(&shizuku_esp_ble_diag.controller_malloc_first_failed_caps,
                                        &expected, caps, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
    }
    return result;
}

void shizuku_esp_ble_note_unimplemented(uint32_t api_id) {
    uint32_t expected = 0;
    __atomic_compare_exchange_n(&shizuku_esp_ble_diag.first_unimplemented_api, &expected,
                                api_id, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&shizuku_esp_ble_diag.unimplemented_api_calls, 1u, __ATOMIC_RELAXED);
}

void shizuku_esp_ble_note_sync_alloc(uint32_t kind, uint32_t reason,
                                   uint32_t objects_used,
                                   uint32_t static_handles_used,
                                   uint32_t mutex_storage_used) {
    if (kind == 0 || kind > 5) return;
    __atomic_fetch_add(&shizuku_esp_ble_diag.sync_create_attempts[kind - 1], 1u, __ATOMIC_RELAXED);
    if (reason != 0) {
        __atomic_fetch_add(&shizuku_esp_ble_diag.sync_create_failures[kind - 1], 1u, __ATOMIC_RELAXED);
        if (reason <= 7)
            __atomic_fetch_add(&shizuku_esp_ble_diag.sync_failure_reasons[reason - 1], 1u, __ATOMIC_RELAXED);
    }
    shizuku_esp_ble_diag.sync_last_kind = kind;
    shizuku_esp_ble_diag.sync_last_reason = reason;
    shizuku_esp_ble_diag.sync_objects_used = objects_used;
    shizuku_esp_ble_diag.sync_static_handles_used = static_handles_used;
    shizuku_esp_ble_diag.sync_dynamic_mutex_slots_used = mutex_storage_used;
}

void shizuku_esp_ble_note_queue_shape(uint32_t capacity, uint32_t item_size) {
    shizuku_esp_ble_diag.sync_last_queue_capacity = capacity;
    shizuku_esp_ble_diag.sync_last_queue_item_size = item_size;
    const uint64_t requested = static_cast<uint64_t>(capacity) * item_size;
    shizuku_esp_ble_diag.sync_last_queue_requested_bytes =
        requested > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(requested);
}

void shizuku_esp_ble_note_queue_send(uint32_t handle, uint32_t item_size,
                                    uint32_t first_word) {
    shizuku_esp_ble_diag.queue_send_count = shizuku_esp_ble_diag.queue_send_count + 1u;
    shizuku_esp_ble_diag.queue_send_last_handle = handle;
    shizuku_esp_ble_diag.queue_send_last_item_size = item_size;
    shizuku_esp_ble_diag.queue_send_last_first_word = first_word;
    const uint32_t sequence = __atomic_add_fetch(
        &shizuku_esp_ble_diag.queue_payload_log_count, 1u, __ATOMIC_RELAXED);
    volatile uint32_t *record = shizuku_esp_ble_diag.queue_payload_records[
        (sequence - 1u) % 16u];
    record[0] = sequence;
    record[1] = 1;
    record[2] = handle;
    record[3] = item_size;
    record[4] = first_word;
}

void shizuku_esp_ble_note_queue_receive(uint32_t handle, uint32_t item_size,
                                       uint32_t first_word) {
    shizuku_esp_ble_diag.queue_receive_count = shizuku_esp_ble_diag.queue_receive_count + 1u;
    shizuku_esp_ble_diag.queue_receive_last_handle = handle;
    shizuku_esp_ble_diag.queue_receive_last_item_size = item_size;
    shizuku_esp_ble_diag.queue_receive_last_first_word = first_word;
    const uint32_t sequence = __atomic_add_fetch(
        &shizuku_esp_ble_diag.queue_payload_log_count, 1u, __ATOMIC_RELAXED);
    volatile uint32_t *record = shizuku_esp_ble_diag.queue_payload_records[
        (sequence - 1u) % 16u];
    record[0] = sequence;
    record[1] = 2;
    record[2] = handle;
    record[3] = item_size;
    record[4] = first_word;
}

static volatile shizuku_esp_ble_task_diag_t *shizuku_esp_ble_find_task_diag(uint32_t task_id) {
    for (auto &record : shizuku_esp_ble_diag.task_diag)
        if (record.task_id == task_id && task_id != 0) return &record;
    return nullptr;
}

static volatile shizuku_esp_ble_task_diag_t *shizuku_esp_ble_add_task_diag(uint32_t task_id) {
    if (!task_id) return nullptr;
    if (auto *found = shizuku_esp_ble_find_task_diag(task_id)) return found;
    for (auto &record : shizuku_esp_ble_diag.task_diag) {
        if (record.task_id == 0) {
            record.task_id = task_id;
            shizuku_esp_ble_diag.task_diag_count = shizuku_esp_ble_diag.task_diag_count + 1u;
            return &record;
        }
    }
    shizuku_esp_ble_diag.task_diag_overflow = shizuku_esp_ble_diag.task_diag_overflow + 1u;
    return nullptr;
}

void shizuku_esp_ble_note_task_create(uint32_t task_id, const char *name,
                                    uint32_t success, uint32_t priority) {
    shizuku_esp_ble_diag.task_create_attempts = shizuku_esp_ble_diag.task_create_attempts + 1u;
    if (!success) {
        shizuku_esp_ble_diag.task_create_failures = shizuku_esp_ble_diag.task_create_failures + 1u;
        return;
    }
    shizuku_esp_ble_diag.task_create_successes = shizuku_esp_ble_diag.task_create_successes + 1u;
    auto *record = shizuku_esp_ble_add_task_diag(task_id);
    if (record) {
        const size_t index = static_cast<size_t>(record - shizuku_esp_ble_diag.task_diag);
        if (index < 8) shizuku_esp_ble_diag.sched_task_priority[index] = priority;
        bool name_ended = name == nullptr;
        for (size_t i = 0; i < sizeof(record->name); ++i) {
            record->name[i] = name_ended ? '\0' : name[i];
            if (record->name[i] == '\0') name_ended = true;
        }
        record->create_count = record->create_count + 1u;
    }
    if (name && std::strcmp(name, "esp_timer") == 0) {
        shizuku_esp_ble_diag.esp_timer_task_id = task_id;
        shizuku_esp_ble_diag.esp_timer_task_create_count = shizuku_esp_ble_diag.esp_timer_task_create_count + 1u;
    }
}

void shizuku_esp_ble_note_task_scheduled(uint32_t task_id, uint32_t priority,
                                        uint32_t wake_to_run_us,
                                        uint32_t clock_skew) {
    if (clock_skew)
        shizuku_esp_ble_diag.sched_latency_clock_skew_count += 1u;
    auto *record = shizuku_esp_ble_find_task_diag(task_id);
    if (!record) return;
    const size_t index = static_cast<size_t>(record - shizuku_esp_ble_diag.task_diag);
    if (index >= 8) return;
    shizuku_esp_ble_diag.sched_task_priority[index] = priority;
    shizuku_esp_ble_diag.sched_task_ready_count[index] += 1u;
    shizuku_esp_ble_diag.sched_task_last_wake_to_run_us[index] = wake_to_run_us;
    if (wake_to_run_us > shizuku_esp_ble_diag.sched_task_max_wake_to_run_us[index])
        shizuku_esp_ble_diag.sched_task_max_wake_to_run_us[index] = wake_to_run_us;
    if (g_ble_connection_active &&
        task_id == shizuku_esp_ble_diag.nimble_host_task_id &&
        wake_to_run_us > shizuku_esp_ble_diag.ble_conn_nimble_host_max_wake_to_run_us)
        shizuku_esp_ble_diag.ble_conn_nimble_host_max_wake_to_run_us = wake_to_run_us;
}

extern "C" void shizuku_esp_ble_note_irq_dispatch(uint32_t source,
                                                   uint32_t interrupted_mepc,
                                                   uint32_t task_id) {
    shizuku_esp_ble_note_irq_dispatch_impl(source, interrupted_mepc, task_id);
}

extern "C" void shizuku_esp_ble_note_irq_registered(uint32_t source,
                                                     uint32_t, uint32_t) {
    for (size_t i = 0; i < 16; ++i) {
        if (shizuku_esp_ble_diag.ble_conn_irq_sources[i] == source) return;
    }
    for (size_t i = 0; i < 16; ++i) {
        if (shizuku_esp_ble_diag.ble_conn_irq_sources[i] != UINT32_MAX) continue;
        shizuku_esp_ble_diag.ble_conn_irq_sources[i] = source;
        shizuku_esp_ble_diag.ble_conn_irq_registration_count += 1u;
        return;
    }
}

extern "C" void shizuku_esp_ble_note_irq_unregistered(uint32_t source) {
    for (size_t i = 0; i < 16; ++i) {
        if (shizuku_esp_ble_diag.ble_conn_irq_sources[i] != source) continue;
        shizuku_esp_ble_diag.ble_conn_irq_sources[i] = UINT32_MAX;
        if (shizuku_esp_ble_diag.ble_conn_irq_registration_count)
            shizuku_esp_ble_diag.ble_conn_irq_registration_count -= 1u;
        return;
    }
}

void shizuku_esp_ble_note_task_started(uint32_t task_id) {
    shizuku_esp_ble_diag.task_start_total = shizuku_esp_ble_diag.task_start_total + 1u;
    auto *record = shizuku_esp_ble_add_task_diag(task_id);
    if (record) record->start_count = record->start_count + 1u;
    if (task_id && task_id == shizuku_esp_ble_diag.esp_timer_task_id)
        shizuku_esp_ble_diag.esp_timer_task_start_count = shizuku_esp_ble_diag.esp_timer_task_start_count + 1u;
}

void shizuku_esp_ble_note_notification(uint32_t api_kind, bool delivered) {
    if (api_kind == 0 || api_kind > 7) return;
    __atomic_fetch_add(&shizuku_esp_ble_diag.notify_api_calls[api_kind - 1], 1u, __ATOMIC_RELAXED);
    if (!delivered)
        __atomic_fetch_add(&shizuku_esp_ble_diag.notify_api_no_delivery[api_kind - 1], 1u, __ATOMIC_RELAXED);
}

void shizuku_esp_ble_note_wait(uint32_t task_id, uint32_t kind, uint32_t handle,
                             uint32_t timeout_ticks, uint32_t entering, uint32_t result) {
    size_t slot = 9;
    if (task_id != 0) {
        for (size_t i = 0; i < 8; ++i) {
            if (shizuku_esp_ble_diag.task_diag[i].task_id == task_id) { slot = i + 1; break; }
        }
    } else {
        slot = 0;
    }
    auto &wait = shizuku_esp_ble_diag.wait_diag[slot];
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

void shizuku_esp_ble_note_wait_callsite(uint32_t pc) {
    shizuku_esp_ble_diag.last_queue_sem_caller_pc = pc;
}

void shizuku_esp_ble_note_sync_object(uint32_t id, uint32_t kind, uint32_t creator_pc,
                                     uint32_t raw_handle,
                                     uint32_t recursive,
                                     uint32_t owner, uint32_t recursion, uint32_t count,
                                     uint32_t waiters, uint32_t last_taker,
                                     uint32_t last_giver, uint32_t take_count,
                                     uint32_t give_count, uint32_t highest_waiter,
                                     uint32_t owner_state) {
    if (id >= 24) return;
    volatile uint32_t *row = shizuku_esp_ble_sync_object_diag[id];
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

void shizuku_esp_ble_note_libc_lock(uint32_t lock_address, uint32_t lock_value,
                                   uint32_t api_kind, uint32_t caller_pc) {
    shizuku_esp_ble_libc_lock_diag[0] = lock_address;
    shizuku_esp_ble_libc_lock_diag[1] = lock_value;
    shizuku_esp_ble_libc_lock_diag[2] = api_kind;
    shizuku_esp_ble_libc_lock_diag[3] = caller_pc;
}

void shizuku_esp_ble_note_scheduler(uint32_t event, uint32_t state) {
    if (event == 0) {
        shizuku_esp_ble_scheduler_diag[0] = state;
        return;
    }
    if (event != 1) return;
    shizuku_esp_ble_scheduler_diag[1] = state;
    shizuku_esp_ble_scheduler_diag[2] = shizuku_esp_ble_scheduler_diag[2] + 1u;
    shizuku_esp_ble_scheduler_diag[3] = shizuku_esp_ble_diag.idf_init_current_slot;
    if (shizuku_esp_ble_diag.idf_init_current_slot == 20)
        shizuku_esp_ble_scheduler_diag[4] = state;
}

static bool shizuku_esp_ble_inspect_heap_regions(void) {
    extern uint8_t _data_start;
    extern uint8_t _heap_start;
    auto &d = shizuku_esp_ble_diag;
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
#if defined(SHIZUKU_ESP32_C6)
// C6's IDF table omits C5-only init_pm_flash_freq_limit and
// init_bootloader_offset. Its slot 24 is usb_serial_jtag_conn_status_init,
// which registers a FreeRTOS tick hook and is intentionally not run here.
constexpr uint32_t kExpectedInitCount = 26;
constexpr uint32_t kSecondaryRunMask = (1u << 5) | (1u << 9) | (1u << 23);
#else
constexpr uint32_t kExpectedInitCount = 28;
// C5 slots 23/24 are init_pm_flash_freq_limit and init_coexist.
constexpr uint32_t kSecondaryRunMask = (1u << 5) | (1u << 9) |
    (1u << 23) | (1u << 24);
#endif
constexpr uint32_t kCoreRunMask = (1u << 4) | (1u << 7) | (1u << 8) |
    (1u << 15) | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) |
    (1u << 20) | (1u << 21);

static bool shizuku_esp_ble_run_idf_stage(uint32_t stage) {
    auto &d = shizuku_esp_ble_diag;
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
            shizuku_esp_ble_scheduler_diag[4] = static_cast<uint32_t>(xTaskGetSchedulerState());
        if (slot == 4) {
            if (!shizuku_esp_ble_inspect_heap_regions()) { d.idf_init_error = 2; return false; }
            d.heap_init_state = 1;
            g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::heap_init);
        }
        const esp_err_t rc = entry.fn();
        if (slot == 20)
            shizuku_esp_ble_scheduler_diag[5] = static_cast<uint32_t>(xTaskGetSchedulerState());
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
static unsigned char shizuku_esp_ble_port_storage[sizeof(shizuku::freertos_compat::shizuku_port)];
alignas(shizuku::freertos_compat::domain)
static unsigned char shizuku_esp_ble_domain_storage[sizeof(shizuku::freertos_compat::domain)];

extern "C" int shizuku_esp_ble_pre_core(void)
{
    constexpr uint32_t kMagic = 0x454C4245u; // bytes "EBLE" in little endian
    constexpr int32_t kNotReturned = 0x7fffffff;
    auto *diag_words = reinterpret_cast<volatile uint32_t *>(&shizuku_esp_ble_diag);
    for (uint32_t i = 0; i < sizeof(shizuku_esp_ble_diag_t) / sizeof(uint32_t); ++i) diag_words[i] = 0;
    for (uint32_t i = 0; i < 24; ++i)
        for (uint32_t j = 0; j < 14; ++j) shizuku_esp_ble_sync_object_diag[i][j] = 0;
    for (uint32_t i = 0; i < 4; ++i) shizuku_esp_ble_libc_lock_diag[i] = 0;
    for (uint32_t i = 0; i < 6; ++i) shizuku_esp_ble_scheduler_diag[i] = 0;
    shizuku_esp_ble_scheduler_diag[4] = UINT32_MAX;
    shizuku_esp_ble_scheduler_diag[5] = UINT32_MAX;
    shizuku_esp_ble_diag.magic = kMagic;
    shizuku_esp_ble_diag.stage = 1;
    shizuku_esp_ble_diag.controller_init_rc = kNotReturned;
    shizuku_esp_ble_diag.controller_enable_rc = kNotReturned;
    shizuku_esp_ble_diag.nimble_port_init_rc = kNotReturned;
    shizuku_esp_ble_diag.gap_device_name_rc = kNotReturned;
    shizuku_esp_ble_diag.gatt_count_cfg_rc = kNotReturned;
    shizuku_esp_ble_diag.gatt_add_svcs_rc = kNotReturned;
    shizuku_esp_ble_diag.adv_set_fields_rc = kNotReturned;
    shizuku_esp_ble_diag.adv_start_rc = kNotReturned;
    shizuku_esp_ble_diag.adv_addr_infer_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_connect_fail_status = -1;
    shizuku_esp_ble_diag.ble_disconnect_reason = -1;
    shizuku_esp_ble_diag.adv_callout_init_rc = kNotReturned;
    shizuku_esp_ble_diag.adv_retry_schedule_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_conn_params_find_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_conn_params_update_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_conn_params_update_status = kNotReturned;
    shizuku_esp_ble_diag.ble_default_phy_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_conn_data_len_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_rssi_callout_init_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_rssi_callout_reset_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_tx_last_notify_rc = kNotReturned;
    shizuku_esp_ble_diag.ble_tx_callout_init_rc = kNotReturned;
    g_throughput_enabled = false;
    g_throughput_subscribed = false;
    g_throughput_sequence = 0;
    g_throughput_payload_len = 0;
#ifdef CONFIG_XTAL_FREQ
    shizuku_esp_ble_diag.ble_cfg_main_xtal_mhz = CONFIG_XTAL_FREQ;
#else
    shizuku_esp_ble_diag.ble_cfg_main_xtal_mhz = UINT32_MAX;
#endif
    // IDF v6.1 C5/C6 bt.c default; Shizuku never calls esp_bt_set_lpclk_freq().
    shizuku_esp_ble_diag.ble_idf_default_bt_lpclk_hz =
        shizuku::esp_idf_support::ble_chip::default_bt_lpclk_hz();
#if defined(CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE) && CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE
    shizuku_esp_ble_diag.ble_cfg_phy_cal_data_storage = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_phy_cal_data_storage = 0u;
#endif
#ifdef CONFIG_ESP_PHY_CALIBRATION_MODE
    shizuku_esp_ble_diag.ble_cfg_phy_calibration_mode = CONFIG_ESP_PHY_CALIBRATION_MODE;
#else
    shizuku_esp_ble_diag.ble_cfg_phy_calibration_mode = UINT32_MAX;
#endif
#if defined(CONFIG_BT_LE_SLEEP_ENABLE) && CONFIG_BT_LE_SLEEP_ENABLE
    shizuku_esp_ble_diag.ble_cfg_bt_le_sleep_enable = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_bt_le_sleep_enable = 0u;
#endif
#if defined(CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL) && CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL
    shizuku_esp_ble_diag.ble_cfg_bt_le_lp_clk_src = 1u;
#elif defined(CONFIG_BT_LE_LP_CLK_SRC_DEFAULT) && CONFIG_BT_LE_LP_CLK_SRC_DEFAULT
    shizuku_esp_ble_diag.ble_cfg_bt_le_lp_clk_src = 0u;
#else
    shizuku_esp_ble_diag.ble_cfg_bt_le_lp_clk_src = 2u;
#endif
#if defined(CONFIG_RTC_CLK_SRC_EXT_CRYS) && CONFIG_RTC_CLK_SRC_EXT_CRYS
    shizuku_esp_ble_diag.ble_cfg_rtc_slow_clk_src = 0u;
#elif defined(CONFIG_RTC_CLK_SRC_INT_RC) && CONFIG_RTC_CLK_SRC_INT_RC
    shizuku_esp_ble_diag.ble_cfg_rtc_slow_clk_src = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_rtc_slow_clk_src = 2u;
#endif
#ifdef CONFIG_BT_LE_LL_SCA
    shizuku_esp_ble_diag.ble_cfg_bt_le_sca_ppm = CONFIG_BT_LE_LL_SCA;
#else
    shizuku_esp_ble_diag.ble_cfg_bt_le_sca_ppm = UINT32_MAX;
#endif
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    shizuku_esp_ble_diag.ble_cfg_pm_enable = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_pm_enable = 0u;
#endif
#if defined(CONFIG_FREERTOS_USE_TICKLESS_IDLE) && CONFIG_FREERTOS_USE_TICKLESS_IDLE
    shizuku_esp_ble_diag.ble_cfg_tickless_idle = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_tickless_idle = 0u;
#endif
#if defined(CONFIG_BT_NIMBLE_USE_ESP_TIMER) && CONFIG_BT_NIMBLE_USE_ESP_TIMER
    shizuku_esp_ble_diag.ble_cfg_nimble_use_esp_timer = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_nimble_use_esp_timer = 0u;
#endif
#if defined(CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP) && CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP
    shizuku_esp_ble_diag.ble_cfg_pm_cpu_power_down = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_pm_cpu_power_down = 0u;
#endif
#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    shizuku_esp_ble_diag.ble_cfg_pm_peripheral_power_down = 1u;
#else
    shizuku_esp_ble_diag.ble_cfg_pm_peripheral_power_down = 0u;
#endif
    shizuku_esp_ble_diag.ble_conn_interval = UINT32_MAX;
    shizuku_esp_ble_diag.ble_conn_latency = UINT32_MAX;
    shizuku_esp_ble_diag.ble_conn_supervision_timeout = UINT32_MAX;
    for (size_t i = 0; i < 16; ++i)
        shizuku_esp_ble_diag.ble_conn_irq_sources[i] = UINT32_MAX;
    shizuku_esp_ble_diag.ble_connection_handle = UINT32_MAX;
    shizuku_esp_ble_diag.compat_bind_rc = kNotReturned;
    shizuku_esp_ble_diag.idf_init_current_slot = UINT32_MAX;
    shizuku_esp_ble_diag.idf_init_last_rc = kNotReturned;
    shizuku_esp_ble_diag.nvs_init_rc = kNotReturned;
    shizuku_esp_ble_diag.shim_timeout_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_xTaskCheckForTimeOut));
    shizuku_esp_ble_diag.idf_timeout_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__real_xTaskCheckForTimeOut));
    shizuku_esp_ble_diag.shim_queue_semaphore_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_xQueueSemaphoreTake));
    shizuku_esp_ble_diag.shim_critical_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__wrap_vPortEnterCritical));
    shizuku_esp_ble_diag.idf_critical_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__real_vPortEnterCritical));
    shizuku_esp_ble_diag.shim_timeout_calls = 0;
    shizuku_esp_ble_diag.shim_queue_semaphore_calls = 0;
    shizuku_esp_ble_diag.shim_critical_calls = 0;
    shizuku_esp_ble_diag.first_unimplemented_api = 0;
    shizuku_esp_ble_diag.unimplemented_api_calls = 0;

    auto *port = new (shizuku_esp_ble_port_storage) shizuku::freertos_compat::shizuku_port{};
    auto *domain = new (shizuku_esp_ble_domain_storage) shizuku::freertos_compat::domain(*port);
    shizuku::freertos_compat::shizuku_port::set_clock(&shizuku::esp_idf_support::ble_chip::time_us);
    const BaseType_t bind_rc = shizuku_freertos_compat_bind(
        domain, 0, shizuku::freertos_compat::shizuku_port::kFirstPlainTaskObject,
        CONFIG_FREERTOS_HZ);
    shizuku_esp_ble_diag.compat_bind_rc = bind_rc;
    asm volatile("fence rw,rw" ::: "memory");
    if (bind_rc != pdPASS) {
        shizuku_esp_ble_diag.stage = 0x10;
        return 0;
    }

    shizuku::esp_idf_support::ble_chip::install_traps();
    if (!shizuku_esp_ble_run_idf_stage(ESP_SYSTEM_INIT_STAGE_CORE)) {
        shizuku_esp_ble_diag.stage = 0x20;
        asm volatile("fence rw,rw" ::: "memory");
        return 0;
    }
    shizuku_esp_ble_diag.libc_ctors_state = 1;
    return 1;
}

extern "C" void shizuku_esp_ble_post_core_ctors(void) {
    shizuku_esp_ble_diag.libc_ctors_state = 2;
}

extern "C" [[noreturn]] void shizuku_esp_ble_stage1_start(void)
{
    // CORE callbacks and libc constructors run before the Shizuku task layer
    // is ready. Keep libc locks as pre-scheduler no-ops until SECONDARY init.
    shizuku_freertos_compat_set_scheduler_state(taskSCHEDULER_RUNNING);
    if (shizuku_esp_ble_diag.compat_bind_rc != pdPASS ||
        shizuku_esp_ble_diag.idf_init_error != 0) {
        shizuku_esp_ble_cooperative_wait();
    }
    if (!shizuku_esp_ble_run_idf_stage(ESP_SYSTEM_INIT_STAGE_SECONDARY)) {
        shizuku_esp_ble_diag.stage = 0x30;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }
    const esp_err_t nvs_rc = nvs_flash_init();
    shizuku_esp_ble_diag.nvs_init_rc = static_cast<int32_t>(nvs_rc);
    if (nvs_rc != ESP_OK) {
        shizuku_esp_ble_diag.stage = 0x31;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }
    const int radio_rc = shizuku::esp_idf_support::ble_chip::prepare_radio();
    if (radio_rc != ESP_OK) {
        shizuku_esp_ble_diag.stage = 0x34;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }
    shizuku_esp_ble_set_stage(4);
    shizuku_esp_ble_diag.controller_heap_free_before =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_esp_ble_diag.controller_heap_largest_before =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::controller_init);
    // IDF v6.1 owns controller init/enable and HCI setup inside
    // nimble_port_init(); calling esp_bt_controller_init/enable separately
    // first is a duplicate-init error on the NimBLE path.
    const esp_err_t init_rc = nimble_port_init();
    g_heap_trace_phase = static_cast<uint32_t>(heap_trace_phase::off);
    shizuku_esp_ble_diag.nimble_port_init_rc = static_cast<int32_t>(init_rc);
    shizuku_esp_ble_diag.controller_heap_free_after =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_esp_ble_diag.controller_heap_largest_after =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    shizuku_esp_ble_diag.enable_heap_free_after =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    shizuku_esp_ble_diag.enable_heap_largest_after =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    shizuku::esp_idf_support::ble_chip::capture_radio_state(6u);
    if (init_rc != ESP_OK) {
        shizuku_esp_ble_diag.stage = 0x35;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }

    // nimble_port_init() returns ESP_OK only after its internal controller
    // init/enable and HCI bring-up complete. Their individual internal return
    // values are not exposed by this IDF API.
    shizuku_esp_ble_diag.controller_init_rc = ESP_OK;
    shizuku_esp_ble_diag.controller_enable_rc = ESP_OK;
    // Capture and transparently probe the controller's registered NPL table.
    // This makes the next controller-side eventq_get result observable before
    // r_ble_ll_reset dereferences it, without changing the returned pointer.
    shizuku_esp_ble_install_npl_probe();
    shizuku_esp_ble_set_stage(5);
    ble_hs_cfg.reset_cb = shizuku_esp_ble_on_reset;
    ble_hs_cfg.sync_cb = shizuku_esp_ble_on_sync;
#if SHIZUKU_ESP_BLE_JUST_WORKS_NO_BOND
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    shizuku_esp_ble_diag.ble_security_profile = 1;
#else
    shizuku_esp_ble_diag.ble_security_profile = 0;
#endif

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_appearance_set(kBleAppearance);
    const int name_rc = ble_svc_gap_device_name_set(kBleDeviceName);
    shizuku_esp_ble_diag.gap_device_name_rc = name_rc;
    if (name_rc != 0) {
        shizuku_esp_ble_diag.stage = 0x36;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }

    const int gatt_rc = shizuku_esp_ble_register_gatt();
    if (gatt_rc != 0) {
        shizuku_esp_ble_diag.stage = 0x37;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }

    const uint32_t host_tasks_before = shizuku_esp_ble_diag.task_create_successes;
    nimble_port_freertos_init(shizuku_esp_ble_host_task);
    shizuku_esp_ble_diag.nimble_host_task_create_count =
        shizuku_esp_ble_diag.task_create_successes - host_tasks_before;
    if (shizuku_esp_ble_diag.nimble_host_task_create_count == 0) {
        shizuku_esp_ble_diag.stage = 0x38;
        asm volatile("fence rw,rw" ::: "memory");
        shizuku_esp_ble_cooperative_wait();
    }
    shizuku_esp_ble_cooperative_wait();
}
}
