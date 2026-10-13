#ifndef SHIZUKU_OBJECTS_USB_CDC_HPP
#define SHIZUKU_OBJECTS_USB_CDC_HPP
#include <cstdint>

// ===========================================================================
//  USB CDC を 2 本持つ (docs/03_porting_policy.md D42)
// ===========================================================================
//  ★1 本を診断と GDB で共有していたのが D41 の不具合の根だった。GDB が繋いだ瞬間も
//    [STRESS] が流れていて、最初の応答の前に人間向けの文字が混ざる → GDB は問い直し、
//    こちらは 2 回答える → 以後ずっと 1 つずれる。**混ざりようが無い形**にする。
//      channel 0 = 診断 (printf がここへ出る)
//      channel 1 = GDB stub
//  ★pico_stdio_usb は CDC 1 本前提で、記述子も weak ではないので差し替えられない。
//    そのため USB は自前で持つ。**リセットインターフェース (picotool -f が使う) は
//    必ず残す** — 無くすと固まったときに焼き直せなくなる。
namespace shizuku {
namespace objects {

#if defined(SHIZUKU_BOOT_LOG_WAIT)
enum class boot_diag_stage : uint32_t {
  USB_WAIT = 1,
  KERNEL_INIT,
  OBJECT_TABLE_INIT,
  BOOTSTRAP,
  APP_ENTRY,
  PERIPHERALS,
  FLASHFS,
  SELFTEST,
  SECONDARY_CORE,
  BLE_REGISTER,
  OTA_REGISTER,
  BLE_POLL_START,
  CYW43_INIT,
  CYW43_READY,
  BTSTACK_INIT,
  GATT_REGISTER,
  ADVERTISEMENT_CONFIG,
  HCI_POWER_ON,
  ADVERTISING,
  OTA_START,
  READY,
  ERROR,
};

enum class boot_diag_breadcrumb : uint32_t {
  NONE = 0,
  BLE_POLL_ENTRY = 0x1001,
  BEFORE_CYW43_INIT = 0x1002,
  AFTER_CYW43_INIT = 0x1003,
  BTSTACK_ENTRY = 0x1004,
  BEFORE_GATT_INIT = 0x1005,
  AFTER_GATT_INIT = 0x1006,
  BEFORE_HCI_POWER = 0x1007,
  AFTER_HCI_POWER = 0x1008,
  BEFORE_ADVERTISE_ENABLE = 0x100a,
  AFTER_ADVERTISE_ENABLE = 0x100b,
  AFTER_ADVERTISING_STAGE = 0x100c,
  BEFORE_CYW43_POLL = 0x100d,
  AFTER_CYW43_POLL = 0x100e,
  BEFORE_SLEEP = 0x100f,
  AFTER_SLEEP = 0x1010,
  PACKET_HANDLER_ENTRY = 0x1011,
  PACKET_HANDLER_EXIT = 0x1012,
  ADVERTISE_API_ENTRY = 0x1013,
  ADVERTISE_API_EXIT = 0x1014,
  DIAG_PRINTF_ENTRY = 0x1020,
  DIAG_FORMAT_DONE = 0x1021,
  DIAG_CDC_WRITE = 0x1022,
  DIAG_CDC_WRITTEN = 0x1023,
  DIAG_PRINTF_EXIT = 0x1024,
  ATOMIC_LOCK_ENTRY = 0x1030,
  ATOMIC_LOCK_EXIT = 0x1031,
  ATOMIC_UNLOCK_ENTRY = 0x1036,
  ATOMIC_UNLOCK_EXIT = 0x1037,
  SVC_DISPATCH_ENTRY = 0x1032,
  SVC_DISPATCH_EXIT = 0x1033,
  PENDSV_DISPATCH_ENTRY = 0x1034,
  PENDSV_DISPATCH_EXIT = 0x1035,
  FLASH_SAFE_EXEC_ENTRY = 0x1040,
  FLASH_SAFE_EXEC_EXIT = 0x1041,
  MULTICORE_LOCKOUT_ENTRY = 0x1042,
  MULTICORE_LOCKOUT_EXIT = 0x1043,
  IRQ_DISABLE_ENTRY = 0x1044,
  IRQ_RESTORE_EXIT = 0x1045,
  CORE_IDLE_ENTRY = 0x1050,
  CORE_IDLE_EXIT = 0x1051,
  KOBJ_SCHEDULE_ENTRY = 0x1052,
  KOBJ_PUMP_ENTRY = 0x1054,
  KOBJ_PUMP_EXIT = 0x1055,
  KOBJ_TABLE_LOCK_WAIT = 0x1056,
  KOBJ_TABLE_LOCK_ACQUIRED = 0x1057,
  KOBJ_TABLE_UNLOCK = 0x1058,
  KOBJ_PUMP_LOCK_BUSY = 0x1059,
  BLE_POLL_OWNER = 0x1060,
  BLE_CORE_VIOLATION = 0x1061,
  USB_TASK_LOCK_BUSY = 0x1070,
  USB_TUD_TASK_ENTRY = 0x1071,
  USB_TUD_TASK_EXIT = 0x1072,
  USB_TUD_LOCK_RELEASED = 0x1073,
  USB_TASK_IRQ_EXIT = 0x1074,
};

extern "C" volatile uint32_t shizuku_boot_diag_stage_id;
void usb_cdc_boot_diag_set_stage(boot_diag_stage stage);
void usb_cdc_boot_diag_set_ble(const char *state);
void usb_cdc_boot_diag_set_advertising(bool enabled);
void usb_cdc_boot_diag_set_connections(uint32_t count);
void usb_cdc_boot_diag_poll();
void usb_cdc_boot_diag_crash_arm();
void usb_cdc_boot_diag_flash_selftest(uint32_t phase);
void usb_cdc_boot_diag_flashdiag_begin_boot(bool crash_recovered,
                                            uint32_t image_stamp);
void usb_cdc_boot_diag_crash_report_drain();
void usb_cdc_boot_diag_systick_tick(uint32_t core);
void usb_cdc_boot_diag_irq_capture(const uint32_t *exception_frame);
void usb_cdc_boot_diag_ota_state(uint32_t state);
void usb_cdc_boot_diag_ota_init_begin();
void usb_cdc_boot_diag_ota_erase_progress(uint32_t erased_sectors,
                                          uint32_t last_erase_us);
void usb_cdc_boot_diag_ota_erase_stage(uint32_t stage, uint32_t offset);
void usb_cdc_boot_diag_flash_bus(uint32_t phase, uint32_t busy_mask,
                                 uint32_t xip_read_mask,
                                 uint32_t pio_dreq_mask,
                                 uint32_t first_read_addr,
                                 uint32_t first_write_addr,
                                 uint32_t poll_active,
                                 uint32_t poll_owner_thread,
                                 uint32_t caller_thread,
                                 uint32_t bus_ready, uint32_t cs_high,
                                 uint32_t reject_reason);
void usb_cdc_boot_diag_note_thread_yield(uint32_t thread_id);
void usb_cdc_boot_diag_tlv_begin();
void usb_cdc_boot_diag_tlv_progress();
void usb_cdc_boot_diag_tlv_end();
void usb_cdc_boot_diag_atomic_wait(uint32_t core, uintptr_t address,
                                   uintptr_t caller_pc, uint32_t thread_id,
                                   uint32_t ipsr);
void usb_cdc_boot_diag_atomic_acquired(uint32_t core, uintptr_t address,
                                       uintptr_t caller_pc, uint32_t thread_id,
                                       uint32_t ipsr, uint32_t primask);
void usb_cdc_boot_diag_atomic_release_begin(uint32_t core);
void usb_cdc_boot_diag_atomic_released(uint32_t core);
void usb_cdc_boot_diag_crash_report_capture(const uint32_t *scratch);
void usb_cdc_boot_diag_crash_report_poll();
void usb_cdc_boot_diag_breadcrumb(boot_diag_breadcrumb id);
void usb_cdc_boot_diag_breadcrumb_at(boot_diag_breadcrumb id,
                                     uintptr_t return_pc);
#endif

// USB を立ち上げ、診断を channel 0 へ結ぶ。ブート直後に 1 回だけ呼ぶ。
void usb_cdc_init();

// channel 1 (GDB 用) の読み書き。★-1 = 今は何も来ていない。
int usb_cdc_read(uint32_t channel);
void usb_cdc_write(uint32_t channel, char value);
void usb_cdc_flush(uint32_t channel);
// あと何バイト積めるか (0 なら、そのまま書くと**捨てられる**)。
uint32_t usb_cdc_write_available(uint32_t channel);
bool usb_cdc_connected(uint32_t channel);
uint32_t usb_cdc_read_available(uint32_t channel);

// 診断 1 行を他コア・tud_task と排他して CDC 0 へ書く (溢れたら false)。
bool usb_cdc_diag_write_line(const char *line, uint32_t length);

// ★panic 用 (board.cpp)。USB (診断出力・picotool のリセット要求) を保つのに
//   要る IRQ (USBCTRL / tud_task を回す user IRQ / それを起こすタイマ IRQ)
//   だけを残して、他の全 IRQ (GPIO・DMA・ペリフェラルすべて) を止める。
//   ここに閉じ込めるのは、どの IRQ が要るかを知っているのが usb_cdc.cpp
//   自身だからで、呼ぶ側 (board.cpp) はその内訳を知らなくてよい。
void usb_cdc_isolate_for_panic();

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_USB_CDC_HPP
