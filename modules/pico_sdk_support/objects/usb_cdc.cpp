// ===========================================================================
//  USB CDC を 2 本持つ (設計の意図は shizuku/objects/usb_cdc.hpp)
// ===========================================================================
//  記述子は pico_stdio_usb の stdio_usb_descriptors.c を土台にしている
//  (Raspberry Pi (Trading) Ltd. / Damien P. George、MIT)。CDC を 2 本に増やし、
//  リセットインターフェースはそのまま残してある。
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN)
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/ssi.h"
#include "pico/cyw43_driver.h"
#include "pico/error.h"
#include "shizuku/objects/flash_map.hpp"
#endif
#if defined(SHIZUKU_CRASH_RECORD)
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/multicore.h"
#endif
#if defined(SHIZUKU_BOOT_LOG_WAIT)
#include <cstddef>
#include <cstdio>
#include <cstring>
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC) && \
    SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC > 0
#include <cstdio>
#include "hardware/structs/scb.h"
#endif
#include "pico/bootrom.h"
#include "pico/stdio/driver.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_BT_TLV_RAM)
#include <cstdio>
#include "btstack_tlv_flash_bank.h"
#include "btstack_tlv_none.h"
#endif
#include "device/usbd_pvt.h"
#include "tusb.h"
#include "shizuku/kernel.hpp"
#include "shizuku/objects/usb_cdc.hpp"
#include "shizuku/objects/flash_ops.hpp"
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/flash.h"
#include "pico/btstack_flash_bank.h"
#endif

#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_BT_TLV_RAM)
// pico-sdk's btstack_cyw43_init() unconditionally calls
// btstack_tlv_flash_bank_init_instance() from setup_tlv(). The firmware link
// wraps that call in RAM-TLV builds so this callback supplies a non-persistent
// store before any flash-bank scan or mutation can occur.
extern "C" const btstack_tlv_t *__wrap_btstack_tlv_flash_bank_init_instance(
    btstack_tlv_flash_bank_t *context,
    const hal_flash_bank_t *hal_flash_bank_impl,
    void *hal_flash_bank_context) {
  (void)context;
  (void)hal_flash_bank_impl;
  (void)hal_flash_bank_context;
  printf("[BT_TLV] RAM store selected; bonds are volatile\n");
  fflush(stdout);
  return btstack_tlv_none_init_instance();
}
#endif

#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN)
#if defined(SHIZUKU_CRASH_RECORD)
static volatile uint32_t g_bt_flash_read_calls = 0;
static volatile uint32_t g_bt_flash_read_last_bank = 0;
static volatile uint32_t g_bt_flash_read_last_offset = 0;
static volatile uint32_t g_bt_flash_read_last_size = 0;
#endif
static const hal_flash_bank_t *g_bt_flash_bank_real = nullptr;
static hal_flash_bank_t g_bt_flash_bank_proxy;
constexpr uint32_t kBtFlashBankSize = PICO_FLASH_BANK_TOTAL_SIZE / 2u;
static_assert(kBtFlashBankSize % FLASH_SECTOR_SIZE == 0);

namespace shizuku::objects {
extern "C" uint32_t shizuku_cyw43_flash_guard_state(
    uint32_t *owner_thread, uint32_t *bus_ready, uint32_t *cs_high);
}

struct flash_dma_snapshot {
  uint32_t busy_mask;
  uint32_t xip_read_mask;
  uint32_t pio_dreq_mask;
  uint32_t first_read_addr;
  uint32_t first_write_addr;
};

static flash_dma_snapshot flash_dma_snapshot_now() {
  flash_dma_snapshot snapshot{};
  uint32_t first_busy_read_addr = UINT32_MAX;
  uint32_t first_busy_write_addr = UINT32_MAX;
  uint32_t first_xip_read_addr = UINT32_MAX;
  uint32_t first_xip_write_addr = UINT32_MAX;
  const uintptr_t xip_bases[] = {XIP_BASE, XIP_NOALLOC_BASE, XIP_NOCACHE_BASE,
                                 XIP_NOCACHE_NOALLOC_BASE};
  for (uint32_t channel = 0; channel < NUM_DMA_CHANNELS; ++channel) {
    const uint32_t ctrl = dma_hw->ch[channel].ctrl_trig;
    if ((ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0)
      continue;
    snapshot.busy_mask |= 1u << channel;
    const uint32_t dreq =
        (ctrl & DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS) >>
        DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB;
    if (dreq <= DREQ_PIO1_RX3)
      snapshot.pio_dreq_mask |= 1u << channel;
    const uint32_t read_addr = dma_hw->ch[channel].read_addr;
    const uint32_t write_addr = dma_hw->ch[channel].write_addr;
    if (first_busy_read_addr == UINT32_MAX) {
      first_busy_read_addr = read_addr;
      first_busy_write_addr = write_addr;
    }
    for (uintptr_t xip_base : xip_bases) {
      if (read_addr >= xip_base &&
          read_addr < xip_base + PICO_FLASH_SIZE_BYTES) {
        snapshot.xip_read_mask |= 1u << channel;
        if (first_xip_read_addr == UINT32_MAX) {
          first_xip_read_addr = read_addr;
          first_xip_write_addr = write_addr;
        }
        break;
      }
    }
  }
  snapshot.first_read_addr = first_xip_read_addr != UINT32_MAX
                                 ? first_xip_read_addr
                                 : first_busy_read_addr;
  snapshot.first_write_addr = first_xip_write_addr != UINT32_MAX
                                  ? first_xip_write_addr
                                  : first_busy_write_addr;
  return snapshot;
}

extern "C" const hal_flash_bank_t *__real_pico_flash_bank_instance(void);

static void bt_flash_bank_read_trace(void *context, int bank, uint32_t offset,
                                     uint8_t *buffer, uint32_t size) {
#if defined(SHIZUKU_CRASH_RECORD)
  const uint32_t calls = ++g_bt_flash_read_calls;
  g_bt_flash_read_last_bank = (uint32_t)bank;
  g_bt_flash_read_last_offset = offset;
  g_bt_flash_read_last_size = size;
  if (watchdog_hw->scratch[0] == 0x43524153u) {
    // Persist before the XIP read, so a stuck callback still leaves the last
    // requested bank/range in the watchdog record.
    watchdog_hw->scratch[3] =
        (watchdog_hw->scratch[3] & 0x0000ffffu) |
        ((calls > 0xffffu ? 0xffffu : calls) << 16);
    watchdog_hw->scratch[7] = 0xe0000000u |
        (((uint32_t)bank & 1u) << 25) |
        ((offset & 0xfffu) << 13) | (size & 0x1fffu);
  }
  g_bt_flash_bank_real->read(context, bank, offset, buffer, size);
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  shizuku::objects::usb_cdc_boot_diag_tlv_progress();
#endif
#else
  g_bt_flash_bank_real->read(context, bank, offset, buffer, size);
#endif
}

static uint32_t bt_bank_offset(int bank) {
  if (bank < 0 || bank > 1)
    shizuku::KERNEL::BOARD::panic("BT TLV invalid flash bank");
  return PICO_FLASH_BANK_STORAGE_OFFSET +
         kBtFlashBankSize * static_cast<uint32_t>(bank);
}

static void bt_flash_bank_erase_service(void *context, int bank) {
  (void)context;
  const int rc = shizuku::objects::flash_ops::erase(
      bt_bank_offset(bank), kBtFlashBankSize);
  if (rc != PICO_OK)
    shizuku::KERNEL::BOARD::panic("BT TLV flash erase rejected");
}

static void bt_flash_bank_write_service(void *context, int bank,
                                        uint32_t offset, const uint8_t *data,
                                        uint32_t size) {
  (void)context;
  if (offset > kBtFlashBankSize || size > kBtFlashBankSize - offset)
    shizuku::KERNEL::BOARD::panic("BT TLV flash write out of range");
  const uint32_t bank_start = bt_bank_offset(bank);
  uint32_t at = 0;
  while (at < size) {
    const uint32_t page_offset = offset + at;
    const uint32_t page_base = page_offset & ~(FLASH_PAGE_SIZE - 1u);
    const uint32_t within = page_offset - page_base;
    const uint32_t amount = (size - at < FLASH_PAGE_SIZE - within)
                                ? size - at
                                : FLASH_PAGE_SIZE - within;
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memcpy(page, reinterpret_cast<const void *>(
                     XIP_BASE + bank_start + page_base),
           sizeof(page));
    memcpy(page + within, data + at, amount);
    const int rc = shizuku::objects::flash_ops::program(
        bank_start + page_base, page, sizeof(page));
    if (rc != PICO_OK)
      shizuku::KERNEL::BOARD::panic("BT TLV flash program rejected");
    at += amount;
  }
}

extern "C" __attribute__((noinline)) const hal_flash_bank_t *
__wrap_pico_flash_bank_instance(void) {
  g_bt_flash_bank_real = __real_pico_flash_bank_instance();
  g_bt_flash_bank_proxy = *g_bt_flash_bank_real;
  g_bt_flash_bank_proxy.read = bt_flash_bank_read_trace;
  g_bt_flash_bank_proxy.erase = bt_flash_bank_erase_service;
  g_bt_flash_bank_proxy.write = bt_flash_bank_write_service;
  return &g_bt_flash_bank_proxy;
}

#endif

namespace shizuku::objects {
extern "C" uint32_t shizuku_cyw43_flash_guard_state(
    uint32_t *owner_thread, uint32_t *bus_ready, uint32_t *cs_high);

extern "C" bool shizuku_flash_operation_allowed() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  uint32_t poll_owner = UINT32_MAX;
  uint32_t bus_ready = 0;
  uint32_t cs_high = 1;
  const uint32_t poll_active =
      shizuku_cyw43_flash_guard_state(&poll_owner, &bus_ready, &cs_high);
  const uint32_t caller_thread = kernel_instance.current_thread_id();
  if ((bus_ready != 0 && cs_high == 0) ||
      (poll_active != 0 && caller_thread != poll_owner))
    return false;
#if defined(SHIZUKU_RP2040)
  const flash_dma_snapshot dma = ::flash_dma_snapshot_now();
  if (dma.xip_read_mask != 0 ||
      (poll_active != 0 && caller_thread == poll_owner &&
       dma.pio_dreq_mask != 0))
    return false;
#endif
#endif
  return true;
}
} // namespace shizuku::objects

// ★★ここが効いているかを**ビルド時に**確かめる。tusb_config.h が届いていないと
//   CDC が 1 本のまま黙って組まれ、記述子だけ 2 本を名乗る = 列挙に失敗する
//   (実機が USB ごと消えて BOOTSEL からしか戻せなくなる)。焼く前に落とす。
static_assert(CFG_TUD_CDC == 2,
              "tusb_config.h が届いていない (CDC が 2 本になっていない)");

namespace shizuku {
namespace objects {
#if defined(SHIZUKU_BOOT_LOG_WAIT)
extern "C" {
__attribute__((used)) volatile uint32_t shizuku_boot_diag_stage_id = 1;
}
#endif
namespace {

// ★VID は変えない。picotool が reset-via-baud でデバイスを探すときに使うので、
//   変えると書き込み手順が壊れる (参照実装が明記している罠)。
// PID は Pico SDK 本家の使い分け (RP2040=0x000a / それ以外=0x0009) に揃える。
// picotool (picoboot_connection.c の PID switch) は 0x0009/0x000a のどちらも
// dr_vidpid_stdio_usb (reset 対象) に分類し、違いは報告するチップ名だけなので
// reset-via-baud には影響しない。PID 固定のままだと RP2040 実機が
// "RP2350 device" と誤認表示される (D.. 参照)。
constexpr uint16_t USBD_VID = 0x2E8A;
#if defined(SHIZUKU_RP2040)
constexpr uint16_t USBD_PID = 0x000a;
#else
constexpr uint16_t USBD_PID = 0x0009;
#endif

enum : uint8_t {
  STR_LANGUAGE = 0,
  STR_MANUFACTURER,
  STR_PRODUCT,
  STR_SERIAL,
  STR_CDC_DIAG,
  STR_CDC_GDB,
  STR_RESET,
};

// インターフェース番号: CDC は 1 本につき 2 つ使う。
enum : uint8_t {
  ITF_CDC_DIAG = 0, // 0, 1
  ITF_CDC_GDB = 2,  // 2, 3
  ITF_RESET = 4,
  ITF_COUNT = 5,
};

constexpr uint8_t EP_DIAG_NOTIFY = 0x81;
constexpr uint8_t EP_DIAG_OUT = 0x02;
constexpr uint8_t EP_DIAG_IN = 0x82;
constexpr uint8_t EP_GDB_NOTIFY = 0x83;
constexpr uint8_t EP_GDB_OUT = 0x04;
constexpr uint8_t EP_GDB_IN = 0x84;

constexpr uint32_t RESET_DESC_LEN = 9;
constexpr uint32_t CONFIG_TOTAL_LEN =
    TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN + RESET_DESC_LEN;

const tusb_desc_device_t g_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // ★複合デバイス (CDC が 2 本 + vendor) なので IAD を使う。ここを CDC クラスに
    //   するとホストが 1 本目しか見ない。
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define SHIZUKU_RESET_DESCRIPTOR(itf, str)                                     \
  9, TUSB_DESC_INTERFACE, itf, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC,               \
      RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, str,

const uint8_t g_config_descriptor[CONFIG_TOTAL_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, STR_LANGUAGE, CONFIG_TOTAL_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_CDC_DIAG, STR_CDC_DIAG, EP_DIAG_NOTIFY, 8,
                       EP_DIAG_OUT, EP_DIAG_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_CDC_GDB, STR_CDC_GDB, EP_GDB_NOTIFY, 8, EP_GDB_OUT,
                       EP_GDB_IN, 64),
    SHIZUKU_RESET_DESCRIPTOR(ITF_RESET, STR_RESET)};

char g_serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
#if defined(SHIZUKU_RP2040)
// The table is immutable; keep its pointer array in flash on the smaller SRAM target.
using usb_string_entry = const char *const;
#else
using usb_string_entry = const char *;
#endif
usb_string_entry g_strings[] = {
    [STR_LANGUAGE] = "",
    [STR_MANUFACTURER] = "Shizuku",
    [STR_PRODUCT] = "Shizuku",
    [STR_SERIAL] = g_serial,
    // ★名前を分けておく。ホスト側でどちらが GDB 用かを目で選べるようにするため。
    //   GDB 側は "Shizuku" を含めない — SWD は使っていない (D40: DebugMonitor +
    //   GDB の Remote Serial Protocol を CDC 越しに喋るだけ) ので、"RSP" の方が
    //   "GDB Server" より実態に即して specific。
    [STR_CDC_DIAG] = "Shizuku diagnostics",
    [STR_CDC_GDB] = "GDB RSP",
    [STR_RESET] = "Reset",
};
uint16_t g_string_buffer[32];

// ---- tud_task を回す --------------------------------------------------------
// ★スレッドから回さない。スレッドが固まると USB ごと死に、**焼き直せなくなる**。
//   最低優先度のユーザー IRQ をタイマで起こす。IRQ は lock の取得時だけ短く
//   割込みを止め、tud_task 自体は SysTick / USBCTRL IRQ を止めずに実行する。
uint8_t g_task_irq;

// ★診断の CDC FIFO には 2 コアと tud_task (core 0 の IRQ) が同時に触れる。
//   行単位で排他するハードウェア spinlock。tud_task は取れなければその回を
//   飛ばし (1ms 後にまた来る)、書き手は待ちを打ち切って行を捨てる (D42)。
//   どちらも無限に待たないのは、panic で止められたコアが保持したままでも
//   panic の USB 出力が詰まらないようにするため。
spin_lock_t *g_diag_lock;
__attribute__((noinline)) uint32_t diag_irq_save() {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  const uintptr_t return_pc = (uintptr_t)__builtin_return_address(0);
  usb_cdc_boot_diag_breadcrumb_at(boot_diag_breadcrumb::IRQ_DISABLE_ENTRY,
                                  return_pc);
#endif
  return save_and_disable_interrupts();
}
__attribute__((noinline)) void diag_irq_restore(uint32_t state) {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  const uintptr_t return_pc = (uintptr_t)__builtin_return_address(0);
#endif
  restore_interrupts(state);
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_breadcrumb_at(boot_diag_breadcrumb::IRQ_RESTORE_EXIT,
                                  return_pc);
#endif
}
#if defined(SHIZUKU_BOOT_LOG_WAIT)
#ifndef SHIZUKU_BT_TLV_WATCHDOG_MAX_MS
#define SHIZUKU_BT_TLV_WATCHDOG_MAX_MS 10000u
#endif
#if defined(SHIZUKU_CRASH_RECORD)
constexpr uint32_t kCrashWatchdogTimeoutMs = 3000u;
#endif
const char * volatile g_boot_diag_ble = "off";
volatile uint32_t g_boot_diag_advertising = 0;
volatile uint32_t g_boot_diag_connections = 0;
volatile uint32_t g_boot_diag_irq_ticks = 0;
volatile bool g_boot_diag_heartbeat_pending = false;
volatile uint32_t g_boot_diag_systick_ticks[2] = {};
volatile uint32_t g_boot_diag_usb_irq_samples = 0;
#if defined(SHIZUKU_CRASH_RECORD)
struct flash_diag_record_t {
  uint32_t seq;
  uint32_t phase;
  uint32_t stage;
  uint32_t erase_rc;
  uint32_t program_rc;
  uint32_t verify_ok;
  uint32_t offset;
  uint32_t start_us;
  uint32_t erase_call_us;
  uint32_t erase_done_us;
  uint32_t program_call_us;
  uint32_t program_done_us;
  uint32_t done_us;
  uint32_t sys_hz;
  uint32_t peri_hz;
  uint32_t ssi_baudr;
  uint32_t ssi_ctrlr0;
  uint32_t ssi_spi_ctrlr0;
  uint32_t primask;
  uint32_t ipsr;
  uint32_t icsr;
  uint32_t sio_irq_pending;
  uint32_t sio_fifo_st;
  uint32_t sio_irq_enabled;
  uint32_t sio_irq_priority;
  uint32_t shpr2;
  uint32_t shpr3;
  uint32_t boot2_expected_checksum;
  uint32_t boot2_checksum;
};

struct ota_crash_diag_t {
  uint32_t magic;
  uint32_t run_seq;
  uint32_t ota_state;
  uint32_t erased_sectors;
  uint32_t last_erase_us;
  uint32_t erase_stage;
  uint32_t erase_offset;
  uint32_t erase_seq;
  uint32_t erase_record_seq;
  uint32_t erase_stage_at_us;
  uint32_t erase_started_us;
  uint32_t erase_safe_us;
  uint32_t erase_flash_call_us;
  uint32_t erase_callback_done_us;
  uint32_t erase_return_us;
  uint32_t dma_pre_busy_mask;
  uint32_t dma_pre_xip_read_mask;
  uint32_t dma_pre_pio_dreq_mask;
  uint32_t dma_pre_read_addr;
  uint32_t dma_pre_write_addr;
  uint32_t dma_safe_busy_mask;
  uint32_t dma_safe_xip_read_mask;
  uint32_t dma_safe_pio_dreq_mask;
  uint32_t dma_safe_read_addr;
  uint32_t dma_safe_write_addr;
  uint32_t dma_call_busy_mask;
  uint32_t dma_call_xip_read_mask;
  uint32_t dma_call_pio_dreq_mask;
  uint32_t dma_call_read_addr;
  uint32_t dma_call_write_addr;
  uint32_t cyw43_poll_active;
  uint32_t cyw43_poll_owner_thread;
  uint32_t flash_caller_thread;
  uint32_t cyw43_bus_ready;
  uint32_t cyw43_cs_high;
  uint32_t flash_guard_rejects;
  uint32_t flash_guard_last_reason;
  uint32_t init_started_us;
  uint32_t init_elapsed_ms;
  uint32_t root_feed_at_us;
  uint32_t root_feed_age_ms;
  uint32_t yield2_at_us;
  uint32_t yield2_age_ms;
  uint32_t yield3_at_us;
  uint32_t yield3_age_ms;
  uint32_t tlv_active;
  uint32_t tlv_elapsed_ms;
  uint32_t irq0_sample_seq;
  uint32_t irq0_sample_at_us;
  uint32_t core1_breadcrumb_seq;
  uint32_t core1_breadcrumb_at_us;
  uint32_t atomic_wait_address[2];
  uint32_t atomic_wait_caller[2];
  uint32_t atomic_wait_thread_id[2];
  uint32_t atomic_wait_ipsr[2];
  uint32_t atomic_wait_valid[2];
  uint32_t atomic_owner_address;
  uint32_t atomic_owner_caller;
  uint32_t atomic_owner_thread_id;
  uint32_t atomic_owner_ipsr;
  uint32_t atomic_owner_primask;
  uint32_t atomic_owner_core;
  uint32_t atomic_owner_active;
  uint32_t atomic_owner_releasing;
  flash_diag_record_t flash_diag[2];
};
constexpr uint32_t kOtaCrashDiagMagic = 0x4f544131u; // "OTA1", layout v10
volatile ota_crash_diag_t __uninitialized_ram(g_ota_crash_diag);
struct pending_crash_report_t {
  uint32_t magic;
  uint32_t scratch[8];
  uint32_t next_line;
  uint32_t repeats_left;
  uint32_t run_seq;
  uint32_t ota_state;
  uint32_t erased_sectors;
  uint32_t last_erase_us;
  uint32_t erase_stage;
  uint32_t erase_offset;
  uint32_t erase_seq;
  uint32_t erase_record_seq;
  uint32_t erase_stage_at_us;
  uint32_t erase_started_us;
  uint32_t erase_safe_us;
  uint32_t erase_flash_call_us;
  uint32_t erase_callback_done_us;
  uint32_t erase_return_us;
  uint32_t dma_pre_busy_mask;
  uint32_t dma_pre_xip_read_mask;
  uint32_t dma_pre_pio_dreq_mask;
  uint32_t dma_pre_read_addr;
  uint32_t dma_pre_write_addr;
  uint32_t dma_safe_busy_mask;
  uint32_t dma_safe_xip_read_mask;
  uint32_t dma_safe_pio_dreq_mask;
  uint32_t dma_safe_read_addr;
  uint32_t dma_safe_write_addr;
  uint32_t dma_call_busy_mask;
  uint32_t dma_call_xip_read_mask;
  uint32_t dma_call_pio_dreq_mask;
  uint32_t dma_call_read_addr;
  uint32_t dma_call_write_addr;
  uint32_t cyw43_poll_active;
  uint32_t cyw43_poll_owner_thread;
  uint32_t flash_caller_thread;
  uint32_t cyw43_bus_ready;
  uint32_t cyw43_cs_high;
  uint32_t flash_guard_rejects;
  uint32_t flash_guard_last_reason;
  uint32_t init_started_us;
  uint32_t init_elapsed_ms;
  uint32_t root_feed_at_us;
  uint32_t root_feed_age_ms;
  uint32_t yield2_at_us;
  uint32_t yield2_age_ms;
  uint32_t yield3_at_us;
  uint32_t yield3_age_ms;
  uint32_t tlv_active;
  uint32_t tlv_elapsed_ms;
  uint32_t irq0_sample_seq;
  uint32_t irq0_sample_at_us;
  uint32_t core1_breadcrumb_seq;
  uint32_t core1_breadcrumb_at_us;
  uint32_t atomic_wait_address[2];
  uint32_t atomic_wait_caller[2];
  uint32_t atomic_wait_thread_id[2];
  uint32_t atomic_wait_ipsr[2];
  uint32_t atomic_wait_valid[2];
  uint32_t atomic_owner_address;
  uint32_t atomic_owner_caller;
  uint32_t atomic_owner_thread_id;
  uint32_t atomic_owner_ipsr;
  uint32_t atomic_owner_primask;
  uint32_t atomic_owner_core;
  uint32_t atomic_owner_active;
  uint32_t atomic_owner_releasing;
  flash_diag_record_t flash_diag[2];
};
constexpr uint32_t kPendingCrashReportMagic = 0x43525039u; // "CRP9", layout v9
volatile pending_crash_report_t __uninitialized_ram(g_pending_crash_report);
volatile uint32_t g_ota_init_started_us = 0;
volatile uint32_t g_tlv_started_us = 0;
volatile uint32_t g_tlv_progress_generation = 0;
volatile uint32_t g_tlv_irq_progress_generation = 0;
uint32_t g_crash_last_fed_tick = 0;
#endif

const char *boot_diag_stage_name(boot_diag_stage stage) {
  switch (stage) {
  case boot_diag_stage::USB_WAIT: return "usb_wait";
  case boot_diag_stage::KERNEL_INIT: return "kernel_init";
  case boot_diag_stage::OBJECT_TABLE_INIT: return "object_table_init";
  case boot_diag_stage::BOOTSTRAP: return "bootstrap";
  case boot_diag_stage::APP_ENTRY: return "app_entry";
  case boot_diag_stage::PERIPHERALS: return "peripherals";
  case boot_diag_stage::FLASHFS: return "flashfs";
  case boot_diag_stage::SELFTEST: return "selftest";
  case boot_diag_stage::SECONDARY_CORE: return "secondary_core";
  case boot_diag_stage::BLE_REGISTER: return "ble_register";
  case boot_diag_stage::OTA_REGISTER: return "ota_register";
  case boot_diag_stage::BLE_POLL_START: return "ble_poll_start";
  case boot_diag_stage::CYW43_INIT: return "cyw43_init";
  case boot_diag_stage::CYW43_READY: return "cyw43_ready";
  case boot_diag_stage::BTSTACK_INIT: return "btstack_init";
  case boot_diag_stage::GATT_REGISTER: return "gatt_register";
  case boot_diag_stage::ADVERTISEMENT_CONFIG: return "adv_config";
  case boot_diag_stage::HCI_POWER_ON: return "hci_power_on";
  case boot_diag_stage::ADVERTISING: return "advertising";
  case boot_diag_stage::OTA_START: return "ota_start";
  case boot_diag_stage::READY: return "ready";
  case boot_diag_stage::ERROR: return "error";
  }
  return "unknown";
}

void boot_diag_emit(const char *prefix) {
  const char *stage;
  const char *ble;
  uint32_t advertising;
  uint32_t connections;
#if defined(SHIZUKU_CRASH_RECORD)
  uint32_t irq0_pc = 0;
  uint32_t irq0_lr = 0;
  uint32_t irq0_thread = 0;
#endif
  const uint32_t irq = diag_irq_save();
  stage = boot_diag_stage_name(
      static_cast<boot_diag_stage>(shizuku_boot_diag_stage_id));
  ble = g_boot_diag_ble;
  advertising = g_boot_diag_advertising;
  connections = g_boot_diag_connections;
  diag_irq_restore(irq);

  char line[448];
  int length;
  if (strcmp(prefix, "[HB]") == 0) {
    uint32_t bt_read_calls = 0;
    uint32_t bt_read_bank = 0;
    uint32_t bt_read_offset = 0;
    uint32_t bt_read_size = 0;
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_CRASH_RECORD)
    bt_read_calls = g_bt_flash_read_calls;
    bt_read_bank = g_bt_flash_read_last_bank;
    bt_read_offset = g_bt_flash_read_last_offset;
    bt_read_size = g_bt_flash_read_last_size;
#endif
#if defined(SHIZUKU_CRASH_RECORD)
    if (watchdog_hw->scratch[0] == 0x43524153u) {
      irq0_pc = watchdog_hw->scratch[4];
      irq0_lr = (watchdog_hw->scratch[6] & ~1u) | 1u;
      irq0_thread = watchdog_hw->scratch[1] >> 16;
    }
#endif
    length = snprintf(line, sizeof(line),
                      "%s stage=%s ble=%s adv=%lu conn=%lu systick0=%lu systick1=%lu usb_irq_samples=%lu feed_thread=%lu flash_ops=parked_core bt_read_calls=%lu bank=%lu offset=%08lx size=%lu irq0_thread=%lu irq0_pc=%08lx irq0_lr=%08lx\n",
                      prefix, stage, ble, (unsigned long)advertising,
                      (unsigned long)connections,
                      (unsigned long)g_boot_diag_systick_ticks[0],
                      (unsigned long)g_boot_diag_systick_ticks[1],
                      (unsigned long)g_boot_diag_usb_irq_samples,
                      (unsigned long)shizuku::kernel_instance.current_thread_id(),
                      (unsigned long)bt_read_calls,
                      (unsigned long)bt_read_bank,
                      (unsigned long)bt_read_offset,
                      (unsigned long)bt_read_size,
#if defined(SHIZUKU_CRASH_RECORD)
                      (unsigned long)irq0_thread,
                      (unsigned long)irq0_pc, (unsigned long)irq0_lr);
#else
                      0ul, 0ul, 0ul);
#endif
  } else {
    length = snprintf(line, sizeof(line),
                      "%s stage=%s ble=%s adv=%lu conn=%lu\n", prefix,
                      stage, ble, (unsigned long)advertising,
                      (unsigned long)connections);
  }
  if (length > 0 && (size_t)length < sizeof(line))
    usb_cdc_diag_write_line(line, (uint32_t)length);

}

void boot_diag_timer_tick() {
  const uint32_t ticks = ++g_boot_diag_irq_ticks;
  if ((ticks % 1000u) == 0)
    g_boot_diag_heartbeat_pending = true;
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    // These ages advance only on the USB task IRQ. They freeze while local
    // PRIMASK blocks that IRQ, so they describe the last IRQ sample, not live
    // elapsed time at a later watchdog reset.
    if (g_ota_crash_diag.root_feed_age_ms != UINT32_MAX)
      ++g_ota_crash_diag.root_feed_age_ms;
    if (g_ota_crash_diag.yield2_age_ms != UINT32_MAX)
      ++g_ota_crash_diag.yield2_age_ms;
    if (g_ota_crash_diag.yield3_age_ms != UINT32_MAX)
      ++g_ota_crash_diag.yield3_age_ms;
    const uint32_t now = time_us_32();
    if (g_ota_init_started_us != 0)
      g_ota_crash_diag.init_elapsed_ms =
          (uint32_t)(now - g_ota_init_started_us) / 1000u;
    if (g_ota_crash_diag.tlv_active != 0) {
      const uint32_t elapsed_ms = (uint32_t)(now - g_tlv_started_us) / 1000u;
      g_ota_crash_diag.tlv_elapsed_ms = elapsed_ms;
      const uint32_t progress = g_tlv_progress_generation;
      // This is a short, operation-progress lease, not a general keepalive:
      // feed only when a completed BT flash read/write advanced the generation,
      // and stop unconditionally at the configured absolute deadline.
      if (progress != g_tlv_irq_progress_generation &&
          elapsed_ms + kCrashWatchdogTimeoutMs <
              SHIZUKU_BT_TLV_WATCHDOG_MAX_MS) {
        g_tlv_irq_progress_generation = progress;
        g_crash_last_fed_tick = ticks;
        watchdog_update();
      }
    }
  }
#endif
}
#endif

void usb_task_irq_handler() {
#if defined(SHIZUKU_CRASH_RECORD)
  // The task IRQ interrupts Shizuku Thread mode on PSP. Save the hardware frame
  // before any calls so the next watchdog record shows where core 0 resumes.
  const uint32_t *exception_frame;
  __asm volatile("mrs %0, psp" : "=r"(exception_frame));
  usb_cdc_boot_diag_irq_capture(exception_frame);
#endif
  const uint32_t state = diag_irq_save();
  const bool lock_acquired =
      g_diag_lock == nullptr || spin_try_lock_unsafe(g_diag_lock);
  diag_irq_restore(state);
  if (lock_acquired) {
    // Do not keep PRIMASK set across TinyUSB. It needs to coexist with the USB
    // controller IRQ, SysTick and the kernel's exception paths. Writers use a
    // try-lock too, so holding the FIFO lock here cannot create a lock wait cycle.
#if defined(SHIZUKU_CRASH_RECORD)
    usb_cdc_boot_diag_breadcrumb(boot_diag_breadcrumb::USB_TUD_TASK_ENTRY);
#endif
    tud_task();
#if defined(SHIZUKU_CRASH_RECORD)
    usb_cdc_boot_diag_breadcrumb(boot_diag_breadcrumb::USB_TUD_TASK_EXIT);
#endif
    if (g_diag_lock != nullptr) {
      spin_unlock_unsafe(g_diag_lock);
#if defined(SHIZUKU_CRASH_RECORD)
      usb_cdc_boot_diag_breadcrumb(
          boot_diag_breadcrumb::USB_TUD_LOCK_RELEASED);
#endif
    }
  } else {
#if defined(SHIZUKU_CRASH_RECORD)
    usb_cdc_boot_diag_breadcrumb(boot_diag_breadcrumb::USB_TASK_LOCK_BUSY);
#endif
  }

#if defined(SHIZUKU_BOOT_LOG_WAIT)
  boot_diag_timer_tick();
#endif
#if defined(SHIZUKU_CRASH_RECORD)
  usb_cdc_boot_diag_breadcrumb(boot_diag_breadcrumb::USB_TASK_IRQ_EXIT);
#endif
}

int64_t usb_task_timer(alarm_id_t, void *) {
  irq_set_pending(g_task_irq);
  return 1000; // 1ms ごと
}

// ---- 診断 (printf) を channel 0 へ結ぶ --------------------------------------
// ★pico_stdio_usb を使わなくなったので、標準出力の行き先は自分で用意する。
void diag_out_chars(const char *buffer, int length) {
  usb_cdc_diag_write_line(buffer, (uint32_t)length);
}
void diag_out_flush() {
  const uint32_t state = diag_irq_save();
  const bool lock_acquired =
      g_diag_lock != nullptr && spin_try_lock_unsafe(g_diag_lock);
  diag_irq_restore(state);
  if (!lock_acquired)
    return;
  tud_cdc_n_write_flush(0);
  spin_unlock_unsafe(g_diag_lock);
}
int diag_in_chars(char *buffer, int length) {
  int taken = 0;
  while (taken < length && tud_cdc_n_available(0))
    buffer[taken++] = (char)tud_cdc_n_read_char(0);
  return taken > 0 ? taken : PICO_ERROR_NO_DATA;
}

stdio_driver_t g_diag_driver = {
    .out_chars = diag_out_chars,
    .out_flush = diag_out_flush,
    .in_chars = diag_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

} // namespace

void usb_cdc_init() {
  if (g_diag_lock == nullptr)
    g_diag_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
  pico_get_unique_board_id_string(g_serial, sizeof(g_serial));
  tusb_init();
  g_task_irq = (uint8_t)user_irq_claim_unused(true);
  irq_set_exclusive_handler(g_task_irq, usb_task_irq_handler);
  irq_set_priority(g_task_irq, PICO_LOWEST_IRQ_PRIORITY);
  irq_set_enabled(g_task_irq, true);
  add_alarm_in_us(1000, usb_task_timer, nullptr, true);
  stdio_set_driver_enabled(&g_diag_driver, true);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC) && \
    SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC > 0
  // This is an observation from normal context after the existing alarm was
  // installed and after the diagnostic stdio driver became active. It does
  // not inspect SDK-private pools[] or claim to identify a later corruption.
  alarm_pool_t *startup_pool = alarm_pool_get_default();
  alarm_pool_timer_t *startup_timer = alarm_pool_get_default_timer();
  const uint startup_alarm = alarm_pool_timer_alarm_num(startup_pool);
  const uint startup_irq = TIMER_ALARM_IRQ_NUM(
      (timer_hw_t *)startup_timer, startup_alarm);
  const irq_handler_t startup_vtable_handler =
      irq_get_vtable_handler(startup_irq);
  const irq_handler_t startup_exclusive_handler =
      irq_get_exclusive_handler(startup_irq);
  printf("[ALARM_DIAG] startup observation only: core=%lu pool=%08lx "
         "timer=%08lx pool_core=%lu alarm=%lu irq=%lu enabled=%u "
         "vtor=%08lx vtable=%08lx exclusive=%08lx task_irq=%u\n",
         (unsigned long)get_core_num(),
         (unsigned long)(uintptr_t)startup_pool,
         (unsigned long)(uintptr_t)startup_timer,
         (unsigned long)alarm_pool_core_num(startup_pool),
         (unsigned long)startup_alarm, (unsigned long)startup_irq,
         (unsigned)irq_is_enabled(startup_irq),
         (unsigned long)scb_hw->vtor,
         (unsigned long)(uintptr_t)startup_vtable_handler,
         (unsigned long)(uintptr_t)startup_exclusive_handler,
         (unsigned)g_task_irq);
#endif
}

// 1 行を spinlock の下でまとめて CDC 0 へ書く。\n は \r\n にする。
bool usb_cdc_diag_write_line(const char *line, uint32_t length) {
  const uint32_t state = diag_irq_save();
  const bool lock_acquired =
      g_diag_lock != nullptr && spin_try_lock_unsafe(g_diag_lock);
  diag_irq_restore(state);
  if (!lock_acquired)
    return false;

  // The lock serializes TinyUSB's CDC task and diagnostic writes. Keep PRIMASK
  // open while copying bytes/flushing the FIFO so SysTick and USBCTRL can run.
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
  uint32_t extra_cr = 0;
  for (uint32_t index = 0; index < length; ++index)
    if (line[index] == '\n' && (index == 0 || line[index - 1] != '\r'))
      ++extra_cr;
  const uint32_t needed = length + extra_cr;
#else
  const uint32_t needed = length;
#endif
  uint32_t available = tud_cdc_n_write_available(0);
  if (available < needed) {
    tud_cdc_n_write_flush(0);
    available = tud_cdc_n_write_available(0);
  }
  const bool written = available >= needed;
  if (written) {
    for (uint32_t index = 0; index < length; ++index) {
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
      if (line[index] == '\n' &&
          (index == 0 || line[index - 1] != '\r'))
        tud_cdc_n_write(0, "\r", 1);
#endif
      tud_cdc_n_write(0, line + index, 1);
    }
  }
  spin_unlock_unsafe(g_diag_lock);
  return written;
}

#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_CRASH_RECORD) && defined(SHIZUKU_BOOT_LOG_WAIT)
namespace {
// Stage 0=not run, 1=started, 2=erase call, 3=erase returned, 4=program
// call, 5=program returned, 6=readback done, 7=failed.
constexpr uint32_t kFlashDiagEraseCall = 2;
constexpr uint32_t kFlashDiagEraseDone = 3;
constexpr uint32_t kFlashDiagProgramCall = 4;
constexpr uint32_t kFlashDiagProgramDone = 5;
constexpr uint32_t kFlashDiagReadbackDone = 6;
constexpr uint32_t kFlashDiagFailed = 7;
constexpr uint32_t kFlashDiagEraseOffset =
    shizuku::objects::flash_map::STAGING_OFFSET +
    shizuku::objects::flash_map::STAGING_BYTES - FLASH_SECTOR_SIZE;
alignas(FLASH_PAGE_SIZE) uint8_t g_flash_diag_page[FLASH_PAGE_SIZE];
volatile uint32_t g_flash_diag_lines_pending[2] = {};

struct flash_diag_mutation {
  uint32_t phase;
  const uint8_t *page;
};

struct flash_diag_once_t {
  uint32_t magic;
  uint32_t image_stamp;
  uint32_t attempted_mask;
};
constexpr uint32_t kFlashDiagOnceMagic = 0x46444e32u; // "FDN2"
volatile flash_diag_once_t __uninitialized_ram(g_flash_diag_once);
uint32_t g_flash_diag_current_image_stamp = 0;
volatile uint32_t g_flash_diag_skip_next = 0;
volatile uint32_t g_flash_diag_skip_report_pending = 0;

uint32_t flash_diag_boot2_expected_checksum() {
  const volatile uint8_t *bytes =
      reinterpret_cast<const volatile uint8_t *>(XIP_BASE);
  uint32_t crc = 0;
  for (uint32_t i = 0; i < 252; ++i) {
    uint8_t byte = bytes[i];
    uint8_t reversed = 0;
    for (uint32_t bit = 0; bit < 8; ++bit) {
      reversed = (uint8_t)((reversed << 1) | (byte & 1u));
      byte >>= 1;
    }
    crc ^= reversed;
    for (uint32_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  uint32_t reversed_crc = 0;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    reversed_crc = (reversed_crc << 1) | (crc & 1u);
    crc >>= 1;
  }
  return reversed_crc;
}

void flash_diag_capture_hw(volatile flash_diag_record_t *record) {
  uint32_t primask;
  uint32_t ipsr;
  __asm volatile("mrs %0, PRIMASK" : "=r"(primask));
  __asm volatile("mrs %0, IPSR" : "=r"(ipsr));
  const uint32_t sio_irq = SIO_FIFO_IRQ_NUM(0);
  const uint32_t irq_mask = 1u << sio_irq;
  record->sys_hz = clock_get_hz(clk_sys);
  record->peri_hz = clock_get_hz(clk_peri);
  record->ssi_baudr = ssi_hw->baudr;
  record->ssi_ctrlr0 = ssi_hw->ctrlr0;
  record->ssi_spi_ctrlr0 = ssi_hw->spi_ctrlr0;
  record->primask = primask;
  record->ipsr = ipsr;
  record->icsr = scb_hw->icsr;
  record->sio_irq_pending = (nvic_hw->ispr & irq_mask) != 0;
  record->sio_fifo_st = sio_hw->fifo_st;
  record->sio_irq_enabled = (nvic_hw->iser & irq_mask) != 0;
  record->sio_irq_priority = irq_get_priority(sio_irq);
  record->shpr2 = *(volatile uint32_t *)0xe000ed1cu;
  record->shpr3 = *(volatile uint32_t *)0xe000ed20u;
  record->boot2_expected_checksum = flash_diag_boot2_expected_checksum();
  record->boot2_checksum =
      *(volatile const uint32_t *)(XIP_BASE + 252u);
}

void flash_diag_mark(uint32_t phase, uint32_t stage) {
  if (phase > 1 || g_ota_crash_diag.magic != kOtaCrashDiagMagic)
    return;
  volatile flash_diag_record_t *record =
      &g_ota_crash_diag.flash_diag[phase];
  const uint32_t odd_seq = (record->seq & ~1u) + 1u;
  record->seq = odd_seq;
  const uint32_t now = time_us_32();
  record->stage = stage;
  if (stage == kFlashDiagEraseCall) {
    record->erase_call_us = now;
    flash_diag_capture_hw(record);
  } else if (stage == kFlashDiagEraseDone) {
    record->erase_done_us = now;
  } else if (stage == kFlashDiagProgramCall) {
    record->program_call_us = now;
    flash_diag_capture_hw(record);
  } else if (stage == kFlashDiagProgramDone) {
    record->program_done_us = now;
  } else if (stage == kFlashDiagReadbackDone ||
             stage == kFlashDiagFailed) {
    record->done_us = now;
  }
  __asm volatile("dmb" ::: "memory");
  record->seq = odd_seq + 1u;
}

void flash_diag_set_result(uint32_t phase, bool erase, int result) {
  volatile flash_diag_record_t *record =
      &g_ota_crash_diag.flash_diag[phase];
  const uint32_t odd_seq = (record->seq & ~1u) + 1u;
  record->seq = odd_seq;
  if (erase)
    record->erase_rc = (uint32_t)result;
  else
    record->program_rc = (uint32_t)result;
  __asm volatile("dmb" ::: "memory");
  record->seq = odd_seq + 1u;
}

int flash_diag_erase(void *opaque) {
  const auto *request = static_cast<const flash_diag_mutation *>(opaque);
  flash_diag_mark(request->phase, kFlashDiagEraseCall);
  const int rc = shizuku::objects::flash_ops::erase(
      kFlashDiagEraseOffset, FLASH_SECTOR_SIZE);
  if (rc != PICO_OK)
    return rc;
  flash_diag_mark(request->phase, kFlashDiagEraseDone);
  return PICO_OK;
}

int flash_diag_program(void *opaque) {
  const auto *request = static_cast<const flash_diag_mutation *>(opaque);
  flash_diag_mark(request->phase, kFlashDiagProgramCall);
  const int rc = shizuku::objects::flash_ops::program(
      kFlashDiagEraseOffset, request->page, FLASH_PAGE_SIZE);
  if (rc != PICO_OK)
    return rc;
  flash_diag_mark(request->phase, kFlashDiagProgramDone);
  return PICO_OK;
}

void flash_diag_emit(uint32_t phase) {
  if (phase > 1 || g_flash_diag_lines_pending[phase] == 0)
    return;
  const volatile flash_diag_record_t *record =
      &g_ota_crash_diag.flash_diag[phase];
  char line[256];
  const uint32_t elapsed = record->done_us != 0
                               ? record->done_us - record->start_us
                               : 0;
  const uint32_t erase_us = record->erase_done_us != 0
                                ? record->erase_done_us - record->erase_call_us
                                : 0;
  const uint32_t program_us =
      record->program_done_us != 0
          ? record->program_done_us - record->program_call_us
          : 0;
  uint32_t pending = g_flash_diag_lines_pending[phase];
  if ((pending & 1u) != 0) {
    const int length = snprintf(
        line, sizeof(line),
        "[CRASH_FLASHDIAG] phase=%s seq=%lu stage=%lu rc=%ld/%ld verify=%lu off=%08lx at=%08lx/%08lx/%08lx us=%lu/%lu/%lu\n",
        phase == 0 ? "pre" : "post", (unsigned long)record->seq,
        (unsigned long)record->stage, (long)(int32_t)record->erase_rc,
        (long)(int32_t)record->program_rc, (unsigned long)record->verify_ok,
        (unsigned long)record->offset, (unsigned long)record->start_us,
        (unsigned long)record->erase_call_us,
        (unsigned long)record->program_call_us, (unsigned long)elapsed,
        (unsigned long)erase_us, (unsigned long)program_us);
    if (length > 0 && (size_t)length < sizeof(line) &&
        usb_cdc_diag_write_line(line, (uint32_t)length))
      pending &= ~1u;
  }
  if ((pending & 2u) != 0) {
    const int length = snprintf(
        line, sizeof(line),
        "[CRASH_FLASHSER] phase=%s clk=%lu/%lu ssi=%08lx/%08lx/%08lx irq=%lu/%lu/%lu/%08lx/%lu/%02lx icsr=%08lx shpr=%08lx/%08lx b2=%08lx/%08lx\n",
        phase == 0 ? "pre" : "post",
        (unsigned long)record->sys_hz, (unsigned long)record->peri_hz,
        (unsigned long)record->ssi_baudr,
        (unsigned long)record->ssi_ctrlr0,
        (unsigned long)record->ssi_spi_ctrlr0,
        (unsigned long)record->primask, (unsigned long)record->ipsr,
        (unsigned long)record->sio_irq_pending,
        (unsigned long)record->sio_fifo_st,
        (unsigned long)record->sio_irq_enabled,
        (unsigned long)record->sio_irq_priority,
        (unsigned long)record->icsr,
        (unsigned long)record->shpr2, (unsigned long)record->shpr3,
        (unsigned long)record->boot2_expected_checksum,
        (unsigned long)record->boot2_checksum);
    if (length > 0 && (size_t)length < sizeof(line) &&
        usb_cdc_diag_write_line(line, (uint32_t)length))
      pending &= ~2u;
  }
  g_flash_diag_lines_pending[phase] = pending;
}

void flash_diag_queue_emit(uint32_t phase) {
  g_flash_diag_lines_pending[phase] = 3u;
  flash_diag_emit(phase);
}

void flash_diag_mark_attempted(uint32_t phase) {
  if (phase > 1)
    return;
  if (g_flash_diag_once.magic != kFlashDiagOnceMagic ||
      g_flash_diag_once.image_stamp != g_flash_diag_current_image_stamp) {
    g_flash_diag_once.magic = 0;
    g_flash_diag_once.image_stamp = g_flash_diag_current_image_stamp;
    g_flash_diag_once.attempted_mask = 0;
    __asm volatile("dmb" ::: "memory");
    g_flash_diag_once.magic = kFlashDiagOnceMagic;
  }
  g_flash_diag_once.attempted_mask |= 1u << phase;
  __asm volatile("dmb" ::: "memory");
}
} // namespace

void usb_cdc_boot_diag_flashdiag_begin_boot(bool crash_recovered,
                                            uint32_t image_stamp) {
  g_flash_diag_current_image_stamp = image_stamp;
  if (!crash_recovered) {
    g_flash_diag_once.magic = 0;
    g_flash_diag_once.image_stamp = image_stamp;
    g_flash_diag_once.attempted_mask = 0;
    g_flash_diag_skip_next = 0;
    g_flash_diag_skip_report_pending = 0;
    __asm volatile("dmb" ::: "memory");
    return;
  }

  bool attempted = false;
  if (g_flash_diag_once.magic == kFlashDiagOnceMagic) {
    if (g_flash_diag_once.image_stamp == image_stamp) {
      attempted = g_flash_diag_once.attempted_mask != 0;
      if (!attempted && g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
        for (uint32_t phase = 0; phase < 2; ++phase)
          attempted = attempted ||
                      g_ota_crash_diag.flash_diag[phase].stage != 0;
      }
    } else {
      g_flash_diag_once.magic = 0;
      g_flash_diag_once.image_stamp = image_stamp;
      g_flash_diag_once.attempted_mask = 0;
    }
  } else if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    for (uint32_t phase = 0; phase < 2; ++phase)
      attempted = attempted ||
                  g_ota_crash_diag.flash_diag[phase].stage != 0;
  }
  g_flash_diag_skip_next = attempted ? 1u : 0u;
  if (attempted) {
    flash_diag_mark_attempted(0);
    flash_diag_mark_attempted(1);
  }
  __asm volatile("dmb" ::: "memory");
}

void usb_cdc_boot_diag_flash_selftest(uint32_t phase) {
  if (phase > 1 || g_ota_crash_diag.magic != kOtaCrashDiagMagic)
    return;
  if (g_flash_diag_skip_next != 0) {
    flash_diag_mark_attempted(phase);
    g_flash_diag_skip_report_pending |= 1u << phase;
    return;
  }
  // The test is synchronous and root cannot feed the watchdog while it runs.
  // Keep enough time to preserve the staged record if flash itself stalls.
  watchdog_enable(8000u, true);
  volatile flash_diag_record_t *record =
      &g_ota_crash_diag.flash_diag[phase];
  const uint32_t odd_seq = (record->seq & ~1u) + 1u;
  record->seq = odd_seq;
  record->phase = phase;
  record->stage = 1;
  record->erase_rc = UINT32_MAX;
  record->program_rc = UINT32_MAX;
  record->verify_ok = 0;
  record->offset = kFlashDiagEraseOffset;
  record->start_us = time_us_32();
  record->erase_call_us = 0;
  record->erase_done_us = 0;
  record->program_call_us = 0;
  record->program_done_us = 0;
  record->done_us = 0;
  flash_diag_capture_hw(record);
  __asm volatile("dmb" ::: "memory");
  record->seq = odd_seq + 1u;
  flash_diag_mark_attempted(phase);

  for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i)
    g_flash_diag_page[i] = (uint8_t)(0x5au ^ (uint8_t)i);
  const flash_diag_mutation mutation{phase, g_flash_diag_page};
  const int erase_rc = flash_diag_erase((void *)&mutation);
  flash_diag_set_result(phase, true, erase_rc);
  if (erase_rc != PICO_OK) {
    flash_diag_mark(phase, kFlashDiagFailed);
    watchdog_enable(3000u, true);
    flash_diag_queue_emit(phase);
    return;
  }
  const int program_rc = flash_diag_program((void *)&mutation);
  flash_diag_set_result(phase, false, program_rc);
  if (program_rc != PICO_OK) {
    flash_diag_mark(phase, kFlashDiagFailed);
    watchdog_enable(3000u, true);
    flash_diag_queue_emit(phase);
    return;
  }
  const volatile uint8_t *readback = reinterpret_cast<const volatile uint8_t *>(
      XIP_BASE + kFlashDiagEraseOffset);
  bool verify_ok = true;
  for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i)
    verify_ok = verify_ok && readback[i] == g_flash_diag_page[i];
  const uint32_t verify_seq = (record->seq & ~1u) + 1u;
  record->seq = verify_seq;
  record->verify_ok = verify_ok ? 1u : 0u;
  __asm volatile("dmb" ::: "memory");
  record->seq = verify_seq + 1u;
  flash_diag_mark(phase, kFlashDiagReadbackDone);
  if (!verify_ok)
    flash_diag_mark(phase, kFlashDiagFailed);
  watchdog_enable(3000u, true);
  flash_diag_queue_emit(phase);
}
#endif // RP2040 CYW43 crash-record flash diagnostic

// ★panic からしか呼ばない (board.cpp)。USB を生かすのに要る 3 本の IRQ だけ
//   残して、他を全部止める:
//     - USBCTRL_IRQ    ハードウェアが実際のイベントを起こす所
//     - g_task_irq      それを受けて tud_task() を回す低優先度 IRQ
//     - タイマ IRQ      g_task_irq を 1ms ごとに起こすアラーム
//   ★GPIO・DMA・ペリフェラルの IRQ は、壊れたカーネルの上でハンドラが
//     勝手に共有状態を触るのを防ぐために止める — SysTick を止めるだけでは
//     スレッド切り替えは止まっても、これらは止まらない。
void usb_cdc_isolate_for_panic() {
  const uint hw_alarm_num =
      alarm_pool_hardware_alarm_num(alarm_pool_get_default());
  const uint timer_irq = TIMER_ALARM_IRQ_NUM(
      (timer_hw_t *)alarm_pool_get_default_timer(), hw_alarm_num);
  for (uint irq = 0; irq < NUM_IRQS; ++irq) {
    if (irq == USBCTRL_IRQ || irq == g_task_irq || irq == timer_irq)
      continue;
    irq_set_enabled(irq, false);
  }
}

int usb_cdc_read(uint32_t channel) {
  if (!tud_cdc_n_available(channel))
    return -1;
  return tud_cdc_n_read_char(channel);
}
void usb_cdc_write(uint32_t channel, char value) {
  tud_cdc_n_write_char(channel, value);
}
void usb_cdc_flush(uint32_t channel) { tud_cdc_n_write_flush(channel); }
// ★あと何バイト積めるか。**満杯のまま書くと tud_cdc_n_write_char は黙って
//   捨てる** — 診断ならそれでよいが (D42「溢れたら捨てる」)、GDB の返事で
//   落とすとプロトコルが壊れる。呼ぶ側が空くのを待てるように口を出す。
uint32_t usb_cdc_write_available(uint32_t channel) {
  return (uint32_t)tud_cdc_n_write_available(channel);
}
uint32_t usb_cdc_read_available(uint32_t channel) {
  return (uint32_t)tud_cdc_n_available(channel);
}
bool usb_cdc_connected(uint32_t channel) {
  return tud_cdc_n_connected(channel);
}

#if defined(SHIZUKU_BOOT_LOG_WAIT)
void usb_cdc_boot_diag_set_stage(boot_diag_stage stage) {
  const uint32_t irq = diag_irq_save();
  shizuku_boot_diag_stage_id = (uint32_t)stage;
#if defined(SHIZUKU_CRASH_RECORD)
  if (watchdog_hw->scratch[0] == 0x43524153u)
    watchdog_hw->scratch[1] = (watchdog_hw->scratch[1] & 0xffff0000u) |
                              (uint32_t)stage | (get_core_num() << 8);
#endif
  diag_irq_restore(irq);
  boot_diag_emit("[BOOT_STAGE]");
}

void usb_cdc_boot_diag_set_ble(const char *state) {
  const uint32_t irq = diag_irq_save();
  g_boot_diag_ble = state;
  diag_irq_restore(irq);
  boot_diag_emit("[BOOT_STATE]");
}

void usb_cdc_boot_diag_set_advertising(bool enabled) {
  const uint32_t irq = diag_irq_save();
  g_boot_diag_advertising = enabled ? 1u : 0u;
  diag_irq_restore(irq);
  boot_diag_emit("[BOOT_STATE]");
}

void usb_cdc_boot_diag_set_connections(uint32_t count) {
  const uint32_t irq = diag_irq_save();
  g_boot_diag_connections = count;
  diag_irq_restore(irq);
  boot_diag_emit("[BOOT_STATE]");
}

void usb_cdc_boot_diag_poll() {
  if (g_boot_diag_heartbeat_pending) {
    g_boot_diag_heartbeat_pending = false;
    if (tud_cdc_n_connected(0)) {
      usb_cdc_boot_diag_crash_report_poll();
      boot_diag_emit("[HB]");
    }
  }
#if defined(SHIZUKU_CRASH_RECORD)
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_CRASH_RECORD)
  for (uint32_t phase = 0; phase < 2; ++phase) {
    if (g_flash_diag_lines_pending[phase] != 0)
      flash_diag_emit(phase);
    if ((g_flash_diag_skip_report_pending & (1u << phase)) != 0) {
      char line[64];
      const int length = snprintf(line, sizeof(line),
                                  "[FLASHDIAG] phase=%s skipped=prior_attempt\n",
                                  phase == 0 ? "pre" : "post");
      if (length > 0 && (size_t)length < sizeof(line) &&
          usb_cdc_diag_write_line(line, (uint32_t)length))
        g_flash_diag_skip_report_pending &= ~(1u << phase);
    }
  }
#endif
  const uint32_t ticks = g_boot_diag_irq_ticks;
  if (g_ota_crash_diag.tlv_active == 0 &&
      ticks != g_crash_last_fed_tick) {
    g_crash_last_fed_tick = ticks;
    if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
      g_ota_crash_diag.root_feed_at_us = time_us_32();
      g_ota_crash_diag.root_feed_age_ms = 0;
    }
    watchdog_update();
  }
#endif
}

void usb_cdc_boot_diag_crash_arm() {
#if defined(SHIZUKU_CRASH_RECORD)
  watchdog_disable();
  uint32_t run_seq =
      g_ota_crash_diag.magic == kOtaCrashDiagMagic
          ? g_ota_crash_diag.run_seq + 1u
          : 1u;
  if (run_seq == 0)
    run_seq = 1;
  // scratch[4]/[6] hold core 0's interrupted PC/LR; [6].bit0 is the SysTick
  // seen flag. scratch[1].high16 holds the interrupted thread ID; scratch[2]
  // low16 is core 0's crumb and high16 holds a flash-safe failure. [3] low16
  // is core 1's crumb and high16 is the saturated BT read count. [5] is core
  // 1's last callsite. scratch[7] holds an atomic target, flash caller LR, or
  // the last BT bank read request (marker E).
  for (uint32_t i = 0; i < 8; ++i)
    watchdog_hw->scratch[i] = 0;
  g_boot_diag_systick_ticks[0] = 0;
  g_boot_diag_systick_ticks[1] = 0;
  g_boot_diag_usb_irq_samples = 0;
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN) && \
    defined(SHIZUKU_BOOT_LOG_WAIT)
  g_flash_diag_lines_pending[0] = 0;
  g_flash_diag_lines_pending[1] = 0;
#endif
  g_ota_crash_diag.magic = 0;
  g_ota_crash_diag.run_seq = run_seq;
  g_ota_crash_diag.ota_state = 0xffffffffu;
  g_ota_crash_diag.erased_sectors = 0;
  g_ota_crash_diag.last_erase_us = 0;
  g_ota_crash_diag.erase_stage = 0;
  g_ota_crash_diag.erase_offset = 0;
  g_ota_crash_diag.erase_seq = 0;
  g_ota_crash_diag.erase_record_seq = 0;
  g_ota_crash_diag.erase_stage_at_us = 0;
  g_ota_crash_diag.erase_started_us = 0;
  g_ota_crash_diag.erase_safe_us = 0;
  g_ota_crash_diag.erase_flash_call_us = 0;
  g_ota_crash_diag.erase_callback_done_us = 0;
  g_ota_crash_diag.erase_return_us = 0;
  g_ota_crash_diag.dma_pre_busy_mask = 0;
  g_ota_crash_diag.dma_pre_xip_read_mask = 0;
  g_ota_crash_diag.dma_pre_pio_dreq_mask = 0;
  g_ota_crash_diag.dma_pre_read_addr = UINT32_MAX;
  g_ota_crash_diag.dma_pre_write_addr = UINT32_MAX;
  g_ota_crash_diag.dma_safe_busy_mask = 0;
  g_ota_crash_diag.dma_safe_xip_read_mask = 0;
  g_ota_crash_diag.dma_safe_pio_dreq_mask = 0;
  g_ota_crash_diag.dma_safe_read_addr = UINT32_MAX;
  g_ota_crash_diag.dma_safe_write_addr = UINT32_MAX;
  g_ota_crash_diag.dma_call_busy_mask = 0;
  g_ota_crash_diag.dma_call_xip_read_mask = 0;
  g_ota_crash_diag.dma_call_pio_dreq_mask = 0;
  g_ota_crash_diag.dma_call_read_addr = UINT32_MAX;
  g_ota_crash_diag.dma_call_write_addr = UINT32_MAX;
  g_ota_crash_diag.cyw43_poll_active = 0;
  g_ota_crash_diag.cyw43_poll_owner_thread = UINT32_MAX;
  g_ota_crash_diag.flash_caller_thread = UINT32_MAX;
  g_ota_crash_diag.cyw43_bus_ready = 0;
  g_ota_crash_diag.cyw43_cs_high = 1;
  g_ota_crash_diag.flash_guard_rejects = 0;
  g_ota_crash_diag.flash_guard_last_reason = 0;
  g_ota_crash_diag.init_started_us = 0;
  g_ota_crash_diag.init_elapsed_ms = 0;
  g_ota_crash_diag.root_feed_at_us = time_us_32();
  g_ota_crash_diag.root_feed_age_ms = 0;
  g_ota_crash_diag.yield2_at_us = 0;
  g_ota_crash_diag.yield2_age_ms = 0;
  g_ota_crash_diag.yield3_at_us = 0;
  g_ota_crash_diag.yield3_age_ms = 0;
  g_ota_crash_diag.tlv_active = 0;
  g_ota_crash_diag.tlv_elapsed_ms = 0;
  g_ota_crash_diag.irq0_sample_seq = 0;
  g_ota_crash_diag.irq0_sample_at_us = 0;
  g_ota_crash_diag.core1_breadcrumb_seq = 0;
  g_ota_crash_diag.core1_breadcrumb_at_us = 0;
  for (uint32_t core = 0; core < 2; ++core) {
    g_ota_crash_diag.atomic_wait_address[core] = 0;
    g_ota_crash_diag.atomic_wait_caller[core] = 0;
    g_ota_crash_diag.atomic_wait_thread_id[core] = 0xffffffffu;
    g_ota_crash_diag.atomic_wait_ipsr[core] = 0;
    g_ota_crash_diag.atomic_wait_valid[core] = 0;
  }
  g_ota_crash_diag.atomic_owner_address = 0;
  g_ota_crash_diag.atomic_owner_caller = 0;
  g_ota_crash_diag.atomic_owner_thread_id = 0xffffffffu;
  g_ota_crash_diag.atomic_owner_ipsr = 0;
  g_ota_crash_diag.atomic_owner_primask = 0xffffffffu;
  g_ota_crash_diag.atomic_owner_core = 0;
  g_ota_crash_diag.atomic_owner_active = 0;
  g_ota_crash_diag.atomic_owner_releasing = 0;
  if (g_flash_diag_skip_next == 0) {
    for (uint32_t phase = 0; phase < 2; ++phase) {
      volatile flash_diag_record_t *record =
          &g_ota_crash_diag.flash_diag[phase];
      record->seq = 0;
      record->phase = phase;
      record->stage = 0;
      record->erase_rc = 0;
      record->program_rc = 0;
      record->verify_ok = 0;
      record->offset = 0;
      record->start_us = 0;
      record->erase_call_us = 0;
      record->erase_done_us = 0;
      record->program_call_us = 0;
      record->program_done_us = 0;
      record->done_us = 0;
      record->sys_hz = 0;
      record->peri_hz = 0;
      record->ssi_baudr = 0;
      record->ssi_ctrlr0 = 0;
      record->ssi_spi_ctrlr0 = 0;
      record->primask = 0;
      record->ipsr = 0;
      record->icsr = 0;
      record->sio_irq_pending = 0;
      record->sio_fifo_st = 0;
      record->sio_irq_enabled = 0;
      record->sio_irq_priority = 0;
      record->shpr2 = 0;
      record->shpr3 = 0;
      record->boot2_expected_checksum = 0;
      record->boot2_checksum = 0;
    }
  }
  g_ota_init_started_us = 0;
  g_tlv_started_us = 0;
  g_tlv_progress_generation = 0;
  g_tlv_irq_progress_generation = 0;
  g_ota_crash_diag.magic = kOtaCrashDiagMagic;
  watchdog_hw->scratch[0] = 0x43524153u; // "CRAS": armed, no fault frame yet
  watchdog_hw->scratch[1] = shizuku_boot_diag_stage_id | (get_core_num() << 8);
  g_crash_last_fed_tick = g_boot_diag_irq_ticks;
  watchdog_enable(3000, true);
#endif
}

void usb_cdc_boot_diag_ota_state(uint32_t state) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic)
    g_ota_crash_diag.ota_state = state;
#else
  (void)state;
#endif
}

void usb_cdc_boot_diag_ota_init_begin() {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    g_ota_init_started_us = time_us_32();
    g_ota_crash_diag.init_started_us = g_ota_init_started_us;
    g_ota_crash_diag.init_elapsed_ms = 0;
    g_ota_crash_diag.erased_sectors = 0;
    g_ota_crash_diag.last_erase_us = 0;
    g_ota_crash_diag.erase_stage = 0;
    g_ota_crash_diag.erase_offset = 0;
    g_ota_crash_diag.erase_seq = 0;
    g_ota_crash_diag.erase_record_seq = 0;
    g_ota_crash_diag.erase_stage_at_us = 0;
    g_ota_crash_diag.erase_started_us = 0;
    g_ota_crash_diag.erase_safe_us = 0;
    g_ota_crash_diag.erase_flash_call_us = 0;
    g_ota_crash_diag.erase_callback_done_us = 0;
    g_ota_crash_diag.erase_return_us = 0;
    g_ota_crash_diag.dma_pre_busy_mask = 0;
    g_ota_crash_diag.dma_pre_xip_read_mask = 0;
    g_ota_crash_diag.dma_pre_pio_dreq_mask = 0;
    g_ota_crash_diag.dma_pre_read_addr = UINT32_MAX;
    g_ota_crash_diag.dma_pre_write_addr = UINT32_MAX;
    g_ota_crash_diag.dma_safe_busy_mask = 0;
    g_ota_crash_diag.dma_safe_xip_read_mask = 0;
    g_ota_crash_diag.dma_safe_pio_dreq_mask = 0;
    g_ota_crash_diag.dma_safe_read_addr = UINT32_MAX;
    g_ota_crash_diag.dma_safe_write_addr = UINT32_MAX;
    g_ota_crash_diag.dma_call_busy_mask = 0;
    g_ota_crash_diag.dma_call_xip_read_mask = 0;
    g_ota_crash_diag.dma_call_pio_dreq_mask = 0;
    g_ota_crash_diag.dma_call_read_addr = UINT32_MAX;
    g_ota_crash_diag.dma_call_write_addr = UINT32_MAX;
    g_ota_crash_diag.flash_guard_rejects = 0;
    g_ota_crash_diag.flash_guard_last_reason = 0;
  }
#endif
}

void usb_cdc_boot_diag_ota_erase_progress(uint32_t erased_sectors,
                                          uint32_t last_erase_us) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    g_ota_crash_diag.erased_sectors = erased_sectors;
    g_ota_crash_diag.last_erase_us = last_erase_us;
  }
#else
  (void)erased_sectors;
  (void)last_erase_us;
#endif
}

void usb_cdc_boot_diag_ota_erase_stage(uint32_t stage, uint32_t offset) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    // Odd erase_record_seq means a watchdog reset interrupted this record
    // update. Even values publish a complete stage transition.
    const uint32_t odd_seq = (g_ota_crash_diag.erase_record_seq & ~1u) + 1u;
    g_ota_crash_diag.erase_record_seq = odd_seq;
    const uint32_t now = time_us_32();
#if defined(SHIZUKU_RP2040) && defined(CYW43_WL_GPIO_LED_PIN)
    if (stage == 2u || stage == 3u) {
      const flash_dma_snapshot dma = flash_dma_snapshot_now();
      uint32_t poll_owner = UINT32_MAX;
      uint32_t bus_ready = 0;
      uint32_t cs_high = 1;
      const uint32_t poll_active = shizuku_cyw43_flash_guard_state(
          &poll_owner, &bus_ready, &cs_high);
      usb_cdc_boot_diag_flash_bus(
          stage == 2u ? 1u : 2u, dma.busy_mask, dma.xip_read_mask,
          dma.pio_dreq_mask,
          dma.first_read_addr, dma.first_write_addr, poll_active, poll_owner,
          shizuku::kernel_instance.current_thread_id(), bus_ready, cs_high, 0);
    }
#endif
    if (stage == 1u) { // OTA erase started
      ++g_ota_crash_diag.erase_seq;
      if (g_ota_crash_diag.erase_seq == 0)
        g_ota_crash_diag.erase_seq = 1;
      g_ota_crash_diag.erase_started_us = now;
      g_ota_crash_diag.erase_safe_us = 0;
      g_ota_crash_diag.erase_callback_done_us = 0;
      g_ota_crash_diag.erase_return_us = 0;
    } else if (stage == 2u) { // safe-zone callback entered
      g_ota_crash_diag.erase_safe_us = now;
    } else if (stage == 3u) { // immediately before the erase call
      g_ota_crash_diag.erase_flash_call_us = now;
    } else if (stage == 4u) { // erase call returned
      g_ota_crash_diag.erase_callback_done_us = now;
    } else if (stage == 5u || stage == 6u) { // flash operation returned
      g_ota_crash_diag.erase_return_us = now;
    }
    g_ota_crash_diag.erase_stage = stage;
    g_ota_crash_diag.erase_offset = offset;
    g_ota_crash_diag.erase_stage_at_us = now;
    g_ota_crash_diag.erase_record_seq = odd_seq + 1u;
  }
#else
  (void)stage;
  (void)offset;
#endif
}

void usb_cdc_boot_diag_flash_bus(uint32_t phase, uint32_t busy_mask,
                                 uint32_t xip_read_mask,
                                 uint32_t pio_dreq_mask,
                                 uint32_t first_read_addr,
                                 uint32_t first_write_addr,
                                 uint32_t poll_active,
                                 uint32_t poll_owner_thread,
                                 uint32_t caller_thread,
                                 uint32_t bus_ready, uint32_t cs_high,
                                 uint32_t reject_reason) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic != kOtaCrashDiagMagic)
    return;
  if (phase == 0u) {
    g_ota_crash_diag.dma_pre_busy_mask = busy_mask;
    g_ota_crash_diag.dma_pre_xip_read_mask = xip_read_mask;
    g_ota_crash_diag.dma_pre_pio_dreq_mask = pio_dreq_mask;
    g_ota_crash_diag.dma_pre_read_addr = first_read_addr;
    g_ota_crash_diag.dma_pre_write_addr = first_write_addr;
  } else if (phase == 1u) {
    g_ota_crash_diag.dma_safe_busy_mask = busy_mask;
    g_ota_crash_diag.dma_safe_xip_read_mask = xip_read_mask;
    g_ota_crash_diag.dma_safe_pio_dreq_mask = pio_dreq_mask;
    g_ota_crash_diag.dma_safe_read_addr = first_read_addr;
    g_ota_crash_diag.dma_safe_write_addr = first_write_addr;
  } else if (phase == 2u) {
    g_ota_crash_diag.dma_call_busy_mask = busy_mask;
    g_ota_crash_diag.dma_call_xip_read_mask = xip_read_mask;
    g_ota_crash_diag.dma_call_pio_dreq_mask = pio_dreq_mask;
    g_ota_crash_diag.dma_call_read_addr = first_read_addr;
    g_ota_crash_diag.dma_call_write_addr = first_write_addr;
  }
  g_ota_crash_diag.cyw43_poll_active = poll_active;
  g_ota_crash_diag.cyw43_poll_owner_thread = poll_owner_thread;
  g_ota_crash_diag.flash_caller_thread = caller_thread;
  g_ota_crash_diag.cyw43_bus_ready = bus_ready;
  g_ota_crash_diag.cyw43_cs_high = cs_high;
  g_ota_crash_diag.flash_guard_last_reason = reject_reason;
  if (reject_reason != 0)
    ++g_ota_crash_diag.flash_guard_rejects;
#else
  (void)phase;
  (void)busy_mask;
  (void)xip_read_mask;
  (void)pio_dreq_mask;
  (void)first_read_addr;
  (void)first_write_addr;
  (void)poll_active;
  (void)poll_owner_thread;
  (void)caller_thread;
  (void)bus_ready;
  (void)cs_high;
  (void)reject_reason;
#endif
}

void usb_cdc_boot_diag_note_thread_yield(uint32_t thread_id) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic != kOtaCrashDiagMagic)
    return;
  if (thread_id == 2) {
    g_ota_crash_diag.yield2_at_us = time_us_32();
    g_ota_crash_diag.yield2_age_ms = 0;
  } else if (thread_id == 3) {
    g_ota_crash_diag.yield3_at_us = time_us_32();
    g_ota_crash_diag.yield3_age_ms = 0;
  }
#else
  (void)thread_id;
#endif
}

void usb_cdc_boot_diag_tlv_begin() {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    g_tlv_started_us = time_us_32();
    g_tlv_progress_generation = 0;
    g_tlv_irq_progress_generation = 0;
    g_ota_crash_diag.tlv_elapsed_ms = 0;
    g_ota_crash_diag.tlv_active = 1;
  }
#endif
}

void usb_cdc_boot_diag_tlv_progress() {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic &&
      g_ota_crash_diag.tlv_active != 0)
    ++g_tlv_progress_generation;
#endif
}

void usb_cdc_boot_diag_tlv_end() {
#if defined(SHIZUKU_CRASH_RECORD)
  g_ota_crash_diag.tlv_active = 0;
#endif
}

void usb_cdc_boot_diag_atomic_wait(uint32_t core, uintptr_t address,
                                   uintptr_t caller_pc, uint32_t thread_id,
                                   uint32_t ipsr) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (core < 2 && g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    g_ota_crash_diag.atomic_wait_valid[core] = 0;
    g_ota_crash_diag.atomic_wait_address[core] = (uint32_t)address;
    g_ota_crash_diag.atomic_wait_caller[core] = (uint32_t)caller_pc;
    g_ota_crash_diag.atomic_wait_thread_id[core] = thread_id;
    g_ota_crash_diag.atomic_wait_ipsr[core] = ipsr;
    g_ota_crash_diag.atomic_wait_valid[core] = 1;
  }
#else
  (void)core;
  (void)address;
  (void)caller_pc;
  (void)thread_id;
  (void)ipsr;
#endif
}

void usb_cdc_boot_diag_atomic_acquired(uint32_t core, uintptr_t address,
                                       uintptr_t caller_pc, uint32_t thread_id,
                                       uint32_t ipsr, uint32_t primask) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (core < 2 && g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
    g_ota_crash_diag.atomic_wait_valid[core] = 0;
    g_ota_crash_diag.atomic_owner_active = 0;
    g_ota_crash_diag.atomic_owner_address = (uint32_t)address;
    g_ota_crash_diag.atomic_owner_caller = (uint32_t)caller_pc;
    g_ota_crash_diag.atomic_owner_thread_id = thread_id;
    g_ota_crash_diag.atomic_owner_ipsr = ipsr;
    g_ota_crash_diag.atomic_owner_primask = primask;
    g_ota_crash_diag.atomic_owner_core = core;
    g_ota_crash_diag.atomic_owner_releasing = 0;
    g_ota_crash_diag.atomic_owner_active = 1;
  }
#else
  (void)core;
  (void)address;
  (void)caller_pc;
  (void)thread_id;
  (void)ipsr;
  (void)primask;
#endif
}

void usb_cdc_boot_diag_atomic_release_begin(uint32_t core) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic &&
      g_ota_crash_diag.atomic_owner_active != 0 &&
      g_ota_crash_diag.atomic_owner_core == core)
    g_ota_crash_diag.atomic_owner_releasing = 1;
#else
  (void)core;
#endif
}

void usb_cdc_boot_diag_atomic_released(uint32_t core) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_ota_crash_diag.magic == kOtaCrashDiagMagic &&
      g_ota_crash_diag.atomic_owner_core == core &&
      g_ota_crash_diag.atomic_owner_releasing != 0) {
    g_ota_crash_diag.atomic_owner_active = 0;
    g_ota_crash_diag.atomic_owner_releasing = 0;
  }
#else
  (void)core;
#endif
}

void usb_cdc_boot_diag_crash_report_capture(const uint32_t *scratch) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (scratch == nullptr)
    return;
  g_pending_crash_report.magic = 0;
  for (uint32_t i = 0; i < 8; ++i)
    g_pending_crash_report.scratch[i] = scratch[i];
  g_pending_crash_report.next_line = 0;
  g_pending_crash_report.repeats_left = 3;
  const bool have_ota = g_ota_crash_diag.magic == kOtaCrashDiagMagic;
#define COPY_CRASH_FIELD(field) \
  g_pending_crash_report.field = have_ota ? g_ota_crash_diag.field : 0xffffffffu
  COPY_CRASH_FIELD(ota_state);
  COPY_CRASH_FIELD(run_seq);
  COPY_CRASH_FIELD(erased_sectors);
  COPY_CRASH_FIELD(last_erase_us);
  COPY_CRASH_FIELD(erase_stage);
  COPY_CRASH_FIELD(erase_offset);
  COPY_CRASH_FIELD(erase_seq);
  COPY_CRASH_FIELD(erase_record_seq);
  COPY_CRASH_FIELD(erase_stage_at_us);
  COPY_CRASH_FIELD(erase_started_us);
  COPY_CRASH_FIELD(erase_safe_us);
  COPY_CRASH_FIELD(erase_flash_call_us);
  COPY_CRASH_FIELD(erase_callback_done_us);
  COPY_CRASH_FIELD(erase_return_us);
  COPY_CRASH_FIELD(dma_pre_busy_mask);
  COPY_CRASH_FIELD(dma_pre_xip_read_mask);
  COPY_CRASH_FIELD(dma_pre_pio_dreq_mask);
  COPY_CRASH_FIELD(dma_pre_read_addr);
  COPY_CRASH_FIELD(dma_pre_write_addr);
  COPY_CRASH_FIELD(dma_safe_busy_mask);
  COPY_CRASH_FIELD(dma_safe_xip_read_mask);
  COPY_CRASH_FIELD(dma_safe_pio_dreq_mask);
  COPY_CRASH_FIELD(dma_safe_read_addr);
  COPY_CRASH_FIELD(dma_safe_write_addr);
  COPY_CRASH_FIELD(dma_call_busy_mask);
  COPY_CRASH_FIELD(dma_call_xip_read_mask);
  COPY_CRASH_FIELD(dma_call_pio_dreq_mask);
  COPY_CRASH_FIELD(dma_call_read_addr);
  COPY_CRASH_FIELD(dma_call_write_addr);
  COPY_CRASH_FIELD(cyw43_poll_active);
  COPY_CRASH_FIELD(cyw43_poll_owner_thread);
  COPY_CRASH_FIELD(flash_caller_thread);
  COPY_CRASH_FIELD(cyw43_bus_ready);
  COPY_CRASH_FIELD(cyw43_cs_high);
  COPY_CRASH_FIELD(flash_guard_rejects);
  COPY_CRASH_FIELD(flash_guard_last_reason);
  COPY_CRASH_FIELD(init_started_us);
  COPY_CRASH_FIELD(init_elapsed_ms);
  COPY_CRASH_FIELD(root_feed_at_us);
  COPY_CRASH_FIELD(root_feed_age_ms);
  COPY_CRASH_FIELD(yield2_at_us);
  COPY_CRASH_FIELD(yield2_age_ms);
  COPY_CRASH_FIELD(yield3_at_us);
  COPY_CRASH_FIELD(yield3_age_ms);
  COPY_CRASH_FIELD(tlv_active);
  COPY_CRASH_FIELD(tlv_elapsed_ms);
  COPY_CRASH_FIELD(irq0_sample_seq);
  COPY_CRASH_FIELD(irq0_sample_at_us);
  COPY_CRASH_FIELD(core1_breadcrumb_seq);
  COPY_CRASH_FIELD(core1_breadcrumb_at_us);
  for (uint32_t core = 0; core < 2; ++core) {
    g_pending_crash_report.atomic_wait_address[core] =
        have_ota ? g_ota_crash_diag.atomic_wait_address[core] : 0;
    g_pending_crash_report.atomic_wait_caller[core] =
        have_ota ? g_ota_crash_diag.atomic_wait_caller[core] : 0;
    g_pending_crash_report.atomic_wait_thread_id[core] =
        have_ota ? g_ota_crash_diag.atomic_wait_thread_id[core] : 0xffffffffu;
    g_pending_crash_report.atomic_wait_ipsr[core] =
        have_ota ? g_ota_crash_diag.atomic_wait_ipsr[core] : 0;
    g_pending_crash_report.atomic_wait_valid[core] =
        have_ota ? g_ota_crash_diag.atomic_wait_valid[core] : 0;
  }
  g_pending_crash_report.atomic_owner_address =
      have_ota ? g_ota_crash_diag.atomic_owner_address : 0;
  g_pending_crash_report.atomic_owner_caller =
      have_ota ? g_ota_crash_diag.atomic_owner_caller : 0;
  g_pending_crash_report.atomic_owner_thread_id =
      have_ota ? g_ota_crash_diag.atomic_owner_thread_id : 0xffffffffu;
  g_pending_crash_report.atomic_owner_ipsr =
      have_ota ? g_ota_crash_diag.atomic_owner_ipsr : 0;
  g_pending_crash_report.atomic_owner_primask =
      have_ota ? g_ota_crash_diag.atomic_owner_primask : 0xffffffffu;
  g_pending_crash_report.atomic_owner_core =
      have_ota ? g_ota_crash_diag.atomic_owner_core : 0;
  g_pending_crash_report.atomic_owner_active =
      have_ota ? g_ota_crash_diag.atomic_owner_active : 0;
  g_pending_crash_report.atomic_owner_releasing =
      have_ota ? g_ota_crash_diag.atomic_owner_releasing : 0;
#define COPY_FLASH_DIAG_FIELD(phase, field)                                  \
  g_pending_crash_report.flash_diag[phase].field =                           \
      have_ota ? g_ota_crash_diag.flash_diag[phase].field : 0xffffffffu
  for (uint32_t phase = 0; phase < 2; ++phase) {
    COPY_FLASH_DIAG_FIELD(phase, seq);
    COPY_FLASH_DIAG_FIELD(phase, phase);
    COPY_FLASH_DIAG_FIELD(phase, stage);
    COPY_FLASH_DIAG_FIELD(phase, erase_rc);
    COPY_FLASH_DIAG_FIELD(phase, program_rc);
    COPY_FLASH_DIAG_FIELD(phase, verify_ok);
    COPY_FLASH_DIAG_FIELD(phase, offset);
    COPY_FLASH_DIAG_FIELD(phase, start_us);
    COPY_FLASH_DIAG_FIELD(phase, erase_call_us);
    COPY_FLASH_DIAG_FIELD(phase, erase_done_us);
    COPY_FLASH_DIAG_FIELD(phase, program_call_us);
    COPY_FLASH_DIAG_FIELD(phase, program_done_us);
    COPY_FLASH_DIAG_FIELD(phase, done_us);
    COPY_FLASH_DIAG_FIELD(phase, sys_hz);
    COPY_FLASH_DIAG_FIELD(phase, peri_hz);
    COPY_FLASH_DIAG_FIELD(phase, ssi_baudr);
    COPY_FLASH_DIAG_FIELD(phase, ssi_ctrlr0);
    COPY_FLASH_DIAG_FIELD(phase, ssi_spi_ctrlr0);
    COPY_FLASH_DIAG_FIELD(phase, primask);
    COPY_FLASH_DIAG_FIELD(phase, ipsr);
    COPY_FLASH_DIAG_FIELD(phase, icsr);
    COPY_FLASH_DIAG_FIELD(phase, sio_irq_pending);
    COPY_FLASH_DIAG_FIELD(phase, sio_fifo_st);
    COPY_FLASH_DIAG_FIELD(phase, sio_irq_enabled);
    COPY_FLASH_DIAG_FIELD(phase, sio_irq_priority);
    COPY_FLASH_DIAG_FIELD(phase, shpr2);
    COPY_FLASH_DIAG_FIELD(phase, shpr3);
    COPY_FLASH_DIAG_FIELD(phase, boot2_expected_checksum);
    COPY_FLASH_DIAG_FIELD(phase, boot2_checksum);
  }
#undef COPY_FLASH_DIAG_FIELD
#undef COPY_CRASH_FIELD
  g_pending_crash_report.magic = kPendingCrashReportMagic;
#else
  (void)scratch;
#endif
}

void usb_cdc_boot_diag_crash_report_poll() {
#if defined(SHIZUKU_CRASH_RECORD)
  if (g_pending_crash_report.magic != kPendingCrashReportMagic ||
      !tud_cdc_n_connected(0))
    return;
  // Queue each preserved line on three connected heartbeat opportunities. A
  // full CDC FIFO leaves the line pending; it is retried without partial writes.
  char line[256];
  uint32_t r[8];
  for (uint32_t i = 0; i < 8; ++i)
    r[i] = g_pending_crash_report.scratch[i];
  const bool hardfault = r[0] == 0x48465254u;
  const uint32_t marker = r[7] & 0xf0000000u;
  int length = 0;
  switch (g_pending_crash_report.next_line) {
  case 0: {
    const uint32_t crumb1 =
        (marker == 0xf0000000u || marker == 0xe0000000u) ? r[2] & 0xffffu
                                                        : r[2];
    length = snprintf(line, sizeof(line),
                      "[CRASH] cause=%s last_core=%lu stage=%lu crumb0=%lu crumb1=%lu systick_core0_seen=%u\n",
                      hardfault ? "hardfault" : "watchdog_timeout",
                      (unsigned long)((r[1] >> 8) & 1u),
                      (unsigned long)(r[1] & 0xffu),
                      (unsigned long)r[2], (unsigned long)crumb1,
                      (!hardfault && (r[6] & 1u)) ? 1u : 0u);
    break;
  }
  case 1: {
    const uint32_t pc = hardfault ? 0u : r[4];
    const uint32_t lr =
        (!hardfault && pc != 0) ? ((r[6] & ~1u) | 1u) : 0u;
    const uint32_t core1_lr = hardfault ? 0u : r[5];
    length = snprintf(line, sizeof(line),
                      "[CRASH_IRQ0] sample_seq=%lu at_us=%08lx t=%lu pc=%08lx lr=%08lx core1_lr=%08lx\n",
                      (unsigned long)g_pending_crash_report.irq0_sample_seq,
                      (unsigned long)g_pending_crash_report.irq0_sample_at_us,
                      (unsigned long)(hardfault ? 0u : r[1] >> 16),
                      (unsigned long)pc, (unsigned long)lr,
                      (unsigned long)core1_lr);
    break;
  }
  case 2: {
    const bool marker_valid = !hardfault && r[7] != 0 &&
                              marker != 0xf0000000u &&
                              marker != 0xe0000000u;
    const uint32_t wait0 = g_pending_crash_report.atomic_wait_valid[0];
    const uint32_t wait1 = g_pending_crash_report.atomic_wait_valid[1];
    const bool owner_active =
        g_pending_crash_report.atomic_owner_active != 0;
    // The address/thread fields survive after valid/active is cleared. Emit
    // sentinels in that case so prior acquisitions cannot look current.
    length = snprintf(line, sizeof(line),
                      "[CRASH_ATOMIC] %s core=%lu target=%08lx wait0=%u/t%lu/i%lu/%08lx/%08lx wait1=%u/t%lu/i%lu/%08lx/%08lx owner=%u:c%lu/t%lu/i%lu/m%lu/%08lx/%08lx rel=%u\n",
                      marker_valid ? "valid" : "valid=0",
                      (unsigned long)(r[7] >> 31),
                      (unsigned long)(r[7] & 0x7fffffffu),
                      (unsigned)wait0,
                      (unsigned long)(wait0 ? g_pending_crash_report.atomic_wait_thread_id[0] : UINT32_MAX),
                      (unsigned long)(wait0 ? g_pending_crash_report.atomic_wait_ipsr[0] : UINT32_MAX),
                      (unsigned long)(wait0 ? g_pending_crash_report.atomic_wait_address[0] : UINT32_MAX),
                      (unsigned long)(wait0 ? g_pending_crash_report.atomic_wait_caller[0] : UINT32_MAX),
                      (unsigned)wait1,
                      (unsigned long)(wait1 ? g_pending_crash_report.atomic_wait_thread_id[1] : UINT32_MAX),
                      (unsigned long)(wait1 ? g_pending_crash_report.atomic_wait_ipsr[1] : UINT32_MAX),
                      (unsigned long)(wait1 ? g_pending_crash_report.atomic_wait_address[1] : UINT32_MAX),
                      (unsigned long)(wait1 ? g_pending_crash_report.atomic_wait_caller[1] : UINT32_MAX),
                      (unsigned)g_pending_crash_report.atomic_owner_active,
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_core : UINT32_MAX),
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_thread_id : UINT32_MAX),
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_ipsr : UINT32_MAX),
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_primask : UINT32_MAX),
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_address : UINT32_MAX),
                      (unsigned long)(owner_active ? g_pending_crash_report.atomic_owner_caller : UINT32_MAX),
                      (unsigned)g_pending_crash_report.atomic_owner_releasing);
    break;
  }
  case 3: {
    static const char *const state_names[] = {
        "IDLE", "HEADER", "DATA", "ZLEN", "ZDATA", "DONE", "FAILED",
        "CSEEK", "CHDR", "CDATA"};
    const uint32_t state = g_pending_crash_report.ota_state;
    const char *state_name =
        state < sizeof(state_names) / sizeof(state_names[0])
            ? state_names[state]
            : "UNKNOWN";
    length = snprintf(line, sizeof(line),
                      "[CRASH_OTA] state=%lu(%s) erased=%lu estage=%lu eoff=%08lx init_tick_ms=%lu root_us=%08lx root_irq_age=%lu y2_us=%08lx y2_irq_age=%lu y3_us=%08lx y3_irq_age=%lu tlv=%lu tlv_ms=%lu\n",
                      (unsigned long)state, state_name,
                      (unsigned long)g_pending_crash_report.erased_sectors,
                      (unsigned long)g_pending_crash_report.erase_stage,
                      (unsigned long)g_pending_crash_report.erase_offset,
                      (unsigned long)g_pending_crash_report.init_elapsed_ms,
                      (unsigned long)g_pending_crash_report.root_feed_at_us,
                      (unsigned long)g_pending_crash_report.root_feed_age_ms,
                      (unsigned long)g_pending_crash_report.yield2_at_us,
                      (unsigned long)g_pending_crash_report.yield2_age_ms,
                      (unsigned long)g_pending_crash_report.yield3_at_us,
                      (unsigned long)g_pending_crash_report.yield3_age_ms,
                      (unsigned long)g_pending_crash_report.tlv_active,
                      (unsigned long)g_pending_crash_report.tlv_elapsed_ms);
    break;
  }
  case 4:
    length = snprintf(
        line, sizeof(line),
        "[CRASH_DMA] run=%lu poll=%lu owner=%lu caller=%lu ready=%lu cs_high=%lu reject=%08lx rejects=%lu pre=%03lx/%03lx/%03lx/%08lx/%08lx safe=%03lx/%03lx/%03lx/%08lx/%08lx call=%03lx/%03lx/%03lx/%08lx/%08lx\n",
        (unsigned long)g_pending_crash_report.run_seq,
        (unsigned long)g_pending_crash_report.cyw43_poll_active,
        (unsigned long)g_pending_crash_report.cyw43_poll_owner_thread,
        (unsigned long)g_pending_crash_report.flash_caller_thread,
        (unsigned long)g_pending_crash_report.cyw43_bus_ready,
        (unsigned long)g_pending_crash_report.cyw43_cs_high,
        (unsigned long)g_pending_crash_report.flash_guard_last_reason,
        (unsigned long)g_pending_crash_report.flash_guard_rejects,
        (unsigned long)g_pending_crash_report.dma_pre_busy_mask,
        (unsigned long)g_pending_crash_report.dma_pre_xip_read_mask,
        (unsigned long)g_pending_crash_report.dma_pre_pio_dreq_mask,
        (unsigned long)g_pending_crash_report.dma_pre_read_addr,
        (unsigned long)g_pending_crash_report.dma_pre_write_addr,
        (unsigned long)g_pending_crash_report.dma_safe_busy_mask,
        (unsigned long)g_pending_crash_report.dma_safe_xip_read_mask,
        (unsigned long)g_pending_crash_report.dma_safe_pio_dreq_mask,
        (unsigned long)g_pending_crash_report.dma_safe_read_addr,
        (unsigned long)g_pending_crash_report.dma_safe_write_addr,
        (unsigned long)g_pending_crash_report.dma_call_busy_mask,
        (unsigned long)g_pending_crash_report.dma_call_xip_read_mask,
        (unsigned long)g_pending_crash_report.dma_call_pio_dreq_mask,
        (unsigned long)g_pending_crash_report.dma_call_read_addr,
        (unsigned long)g_pending_crash_report.dma_call_write_addr);
    break;
  case 5:
    length = snprintf(
        line, sizeof(line),
        "[CRASH_ERASE] run=%lu init_us=%08lx seq=%lu write_seq=%lu stage=%lu offset=%08lx start=%08lx safe=%08lx flash_call=%08lx cb_done=%08lx return=%08lx stage_at=%08lx\n",
        (unsigned long)g_pending_crash_report.run_seq,
        (unsigned long)g_pending_crash_report.init_started_us,
        (unsigned long)g_pending_crash_report.erase_seq,
        (unsigned long)g_pending_crash_report.erase_record_seq,
        (unsigned long)g_pending_crash_report.erase_stage,
        (unsigned long)g_pending_crash_report.erase_offset,
        (unsigned long)g_pending_crash_report.erase_started_us,
        (unsigned long)g_pending_crash_report.erase_safe_us,
        (unsigned long)g_pending_crash_report.erase_flash_call_us,
        (unsigned long)g_pending_crash_report.erase_callback_done_us,
        (unsigned long)g_pending_crash_report.erase_return_us,
        (unsigned long)g_pending_crash_report.erase_stage_at_us);
    break;
  case 6:
    length = snprintf(line, sizeof(line),
                      "[CRASH_FAULT] pc=%08lx lr=%08lx xpsr=%08lx exc_return=%08lx\n",
                      (unsigned long)(hardfault ? r[4] : 0u),
                      (unsigned long)(hardfault ? r[5] : 0u),
                      (unsigned long)(hardfault ? r[6] : 0u),
                      (unsigned long)(hardfault ? r[7] : 0u));
    break;
  case 7: {
    const uint32_t lr = hardfault ? 0u : r[5];
    const uint32_t pc = lr ? (lr & ~1u) - 4u : 0u;
    length = snprintf(line, sizeof(line),
                      "[CRASH_CORE1] sample_seq=%lu at_us=%08lx caller_pc=%08lx caller_lr=%08lx\n",
                      (unsigned long)g_pending_crash_report.core1_breadcrumb_seq,
                      (unsigned long)g_pending_crash_report.core1_breadcrumb_at_us,
                      (unsigned long)pc, (unsigned long)lr);
    break;
  }
  case 8:
    length = snprintf(line, sizeof(line),
                      "[CRASH_FLASH] common_service=park_other_cores\n");
    break;
  case 9: {
    const bool bt_read_error = marker == 0xe0000000u;
    length = snprintf(line, sizeof(line),
                      "[CRASH_BT_READ] calls=%lu bank=%lu offset=%08lx size=%lu\n",
                      (unsigned long)(bt_read_error ? r[3] >> 16 : 0u),
                      (unsigned long)(bt_read_error ? (r[7] >> 25) & 1u : 0u),
                      (unsigned long)(bt_read_error ? (r[7] >> 13) & 0xfffu : 0u),
                      (unsigned long)(bt_read_error ? r[7] & 0x1fffu : 0u));
    break;
  }
  case 10:
  case 11:
  case 12:
  case 13: {
    const uint32_t phase = g_pending_crash_report.next_line < 12u
                               ? g_pending_crash_report.next_line - 10u
                               : g_pending_crash_report.next_line - 12u;
    const volatile flash_diag_record_t &record =
        g_pending_crash_report.flash_diag[phase];
    if (g_pending_crash_report.next_line < 12u) {
      const uint32_t elapsed = record.done_us != 0
                                   ? record.done_us - record.start_us
                                   : 0;
      const uint32_t erase_us = record.erase_done_us != 0
                                    ? record.erase_done_us - record.erase_call_us
                                    : 0;
      const uint32_t program_us =
          record.program_done_us != 0
              ? record.program_done_us - record.program_call_us
              : 0;
      length = snprintf(
          line, sizeof(line),
          "[CRASH_FLASHDIAG] phase=%s seq=%lu stage=%lu rc=%ld/%ld verify=%lu off=%08lx at=%08lx/%08lx/%08lx us=%lu/%lu/%lu\n",
          phase == 0 ? "pre" : "post", (unsigned long)record.seq,
          (unsigned long)record.stage, (long)(int32_t)record.erase_rc,
          (long)(int32_t)record.program_rc, (unsigned long)record.verify_ok,
          (unsigned long)record.offset, (unsigned long)record.start_us,
          (unsigned long)record.erase_call_us,
          (unsigned long)record.program_call_us, (unsigned long)elapsed,
          (unsigned long)erase_us, (unsigned long)program_us);
    } else {
      length = snprintf(
          line, sizeof(line),
          "[CRASH_FLASHSER] phase=%s clk=%lu/%lu ssi=%08lx/%08lx/%08lx irq=%lu/%lu/%lu/%08lx/%lu/%02lx icsr=%08lx shpr=%08lx/%08lx b2=%08lx/%08lx\n",
          phase == 0 ? "pre" : "post", (unsigned long)record.sys_hz,
          (unsigned long)record.peri_hz, (unsigned long)record.ssi_baudr,
          (unsigned long)record.ssi_ctrlr0,
          (unsigned long)record.ssi_spi_ctrlr0,
          (unsigned long)record.primask, (unsigned long)record.ipsr,
          (unsigned long)record.sio_irq_pending,
          (unsigned long)record.sio_fifo_st,
          (unsigned long)record.sio_irq_enabled,
          (unsigned long)record.sio_irq_priority, (unsigned long)record.icsr,
          (unsigned long)record.shpr2, (unsigned long)record.shpr3,
          (unsigned long)record.boot2_expected_checksum,
          (unsigned long)record.boot2_checksum);
    }
    break;
  }
  default:
    length = snprintf(line, sizeof(line), "[CRASH] invalid report line\n");
    break;
  }
  if (length <= 0 || (size_t)length >= sizeof(line) ||
      !usb_cdc_diag_write_line(line, (uint32_t)length))
    return;
  if (--g_pending_crash_report.repeats_left == 0) {
    ++g_pending_crash_report.next_line;
    g_pending_crash_report.repeats_left = 3;
    if (g_pending_crash_report.next_line >= 14)
      g_pending_crash_report.magic = 0;
  }
#endif
}

void usb_cdc_boot_diag_crash_report_drain() {
#if defined(SHIZUKU_CRASH_RECORD)
  if (!tud_cdc_n_connected(0))
    return;
  const uint64_t deadline_us = time_us_64() + 5000000ull;
  while (g_pending_crash_report.magic == kPendingCrashReportMagic &&
         tud_cdc_n_connected(0) && time_us_64() < deadline_us) {
    const uint32_t old_line = g_pending_crash_report.next_line;
    const uint32_t old_repeats = g_pending_crash_report.repeats_left;
    usb_cdc_boot_diag_crash_report_poll();
    if (old_line == g_pending_crash_report.next_line &&
        old_repeats == g_pending_crash_report.repeats_left)
      sleep_ms(1);
  }
#endif
}

void usb_cdc_boot_diag_systick_tick(uint32_t core) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (core < 2 && watchdog_hw->scratch[0] == 0x43524153u) {
    ++g_boot_diag_systick_ticks[core];
    if (core == 0)
      watchdog_hw->scratch[6] |= 1u; // stored in the unused Thumb LR bit
  }
#else
  (void)core;
#endif
}

void usb_cdc_boot_diag_irq_capture(const uint32_t *exception_frame) {
#if defined(SHIZUKU_CRASH_RECORD)
  // This is the frame interrupted by the most recent USB task IRQ. The WDT
  // reset itself does not invoke this capture, so the PC is historical.
  if (get_core_num() == 0 && exception_frame != nullptr &&
      watchdog_hw->scratch[0] == 0x43524153u) {
    ++g_boot_diag_usb_irq_samples;
    const uint32_t odd_seq =
        (g_ota_crash_diag.irq0_sample_seq & ~1u) + 1u;
    if (g_ota_crash_diag.magic == kOtaCrashDiagMagic)
      g_ota_crash_diag.irq0_sample_seq = odd_seq;
    watchdog_hw->scratch[4] = exception_frame[6]; // interrupted PC
    const uint32_t systick_seen = watchdog_hw->scratch[6] & 1u;
    watchdog_hw->scratch[6] = (exception_frame[5] & ~1u) | systick_seen;
    const uint32_t stage_core = watchdog_hw->scratch[1] & 0xffffu;
    const uint32_t thread = shizuku::kernel_instance.current_thread_id();
    watchdog_hw->scratch[1] = stage_core | (thread << 16);
    if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
      g_ota_crash_diag.irq0_sample_at_us = time_us_32();
      g_ota_crash_diag.irq0_sample_seq = odd_seq + 1u;
    }
  }
#else
  (void)exception_frame;
#endif
}

__attribute__((noinline)) void usb_cdc_boot_diag_breadcrumb(
    boot_diag_breadcrumb id) {
#if defined(SHIZUKU_CRASH_RECORD)
  const uintptr_t return_pc = (uintptr_t)__builtin_return_address(0);
  usb_cdc_boot_diag_breadcrumb_at(id, return_pc);
#else
  (void)id;
#endif
}

void usb_cdc_boot_diag_breadcrumb_at(boot_diag_breadcrumb id,
                                     uintptr_t return_pc) {
#if defined(SHIZUKU_CRASH_RECORD)
  if (watchdog_hw->scratch[0] == 0x43524153u) {
    const uint32_t core = get_core_num();
    watchdog_hw->scratch[1] = (watchdog_hw->scratch[1] & 0xffff0000u) |
                              shizuku_boot_diag_stage_id | (core << 8);
    watchdog_hw->scratch[2 + core] =
        (watchdog_hw->scratch[2 + core] & 0xffff0000u) | (uint32_t)id;
    // Core 0's scratch PC/LR hold the interrupted IRQ return context. Keep the
    // core-1 atomic/kernel callsite in its own slot for lock-wait diagnosis.
    if (core == 1) {
      const uint32_t odd_seq =
          (g_ota_crash_diag.core1_breadcrumb_seq & ~1u) + 1u;
      if (g_ota_crash_diag.magic == kOtaCrashDiagMagic)
        g_ota_crash_diag.core1_breadcrumb_seq = odd_seq;
      watchdog_hw->scratch[5] = (uint32_t)return_pc;
      if (g_ota_crash_diag.magic == kOtaCrashDiagMagic) {
        g_ota_crash_diag.core1_breadcrumb_at_us = time_us_32();
        g_ota_crash_diag.core1_breadcrumb_seq = odd_seq + 1u;
      }
    }
  }
#else
  (void)id;
  (void)return_pc;
#endif
}

#endif

} // namespace objects
} // namespace shizuku

// ---- TinyUSB のコールバック (C リンケージ) ----------------------------------
extern "C" {

const uint8_t *tud_descriptor_device_cb(void) {
  return (const uint8_t *)&shizuku::objects::g_device_descriptor;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t) {
  return shizuku::objects::g_config_descriptor;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t) {
  using namespace shizuku::objects;
  uint32_t length = 0;
  if (index == STR_LANGUAGE) {
    g_string_buffer[1] = 0x0409; // 英語 (米国)
    length = 1;
  } else {
    if (index >= sizeof(g_strings) / sizeof(g_strings[0]))
      return nullptr;
    const char *text = g_strings[index];
    while (text[length] != '\0' && length < 31) {
      g_string_buffer[1 + length] = (uint16_t)text[length];
      ++length;
    }
  }
  g_string_buffer[0] =
      (uint16_t)((TUSB_DESC_STRING << 8) | (2 * length + 2));
  return g_string_buffer;
}

// ★★リセットインターフェース (class 0xFF) には**専用のドライバが要る**。
//   記述子で名乗るだけだと、TinyUSB はそのインターフェースを開けるドライバを
//   見つけられず、**構成の確立に失敗して USB ごと列挙されなくなる**
//   (実際にそれで板が消え、BOOTSEL 押下でしか戻せなくなった)。
//   pico_stdio_usb の reset_interface.c と同じ形で、自前のドライバを足す。
static uint8_t g_reset_itf = 0;

static void resetd_init(void) {}
static void resetd_reset(uint8_t) { g_reset_itf = 0; }

static uint16_t resetd_open(uint8_t, tusb_desc_interface_t const *desc,
                            uint16_t max_len) {
  TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == desc->bInterfaceClass &&
                RESET_INTERFACE_SUBCLASS == desc->bInterfaceSubClass &&
                RESET_INTERFACE_PROTOCOL == desc->bInterfaceProtocol,
            0);
  const uint16_t length = sizeof(tusb_desc_interface_t);
  TU_VERIFY(max_len >= length, 0);
  g_reset_itf = desc->bInterfaceNumber;
  return length;
}

static bool resetd_control_xfer_cb(uint8_t, uint8_t stage,
                                   tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP)
    return true;
  if (request->wIndex != g_reset_itf)
    return false;
  if (request->bRequest == RESET_REQUEST_BOOTSEL) {
    reset_usb_boot(0, 0); // 戻らない
    return true;
  }
  return false;
}

static bool resetd_xfer_cb(uint8_t, uint8_t, xfer_result_t, uint32_t) {
  return true;
}

static usbd_class_driver_t const g_reset_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "RESET",
#endif
    .init = resetd_init,
    .reset = resetd_reset,
    .open = resetd_open,
    .control_xfer_cb = resetd_control_xfer_cb,
    .xfer_cb = resetd_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
  *driver_count = 1;
  return &g_reset_driver;
}

// 1200bps タッチ。★これも復旧経路なので残す。
void tud_cdc_line_coding_cb(uint8_t, cdc_line_coding_t const *coding) {
  if (coding->bit_rate == 1200)
    reset_usb_boot(0, 0);
}

} // extern "C"
