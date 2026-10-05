// ===========================================================================
//  USB CDC を 2 本持つ (設計の意図は shizuku/objects/usb_cdc.hpp)
// ===========================================================================
//  記述子は pico_stdio_usb の stdio_usb_descriptors.c を土台にしている
//  (Raspberry Pi (Trading) Ltd. / Damien P. George、MIT)。CDC を 2 本に増やし、
//  リセットインターフェースはそのまま残してある。
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC) && \
    SHIZUKU_USB_ALARM_STARTUP_DIAGNOSTIC > 0
#include <cstdio>
#include "hardware/structs/scb.h"
#include "pico/platform.h"
#endif
#include "pico/bootrom.h"
#include "pico/stdio/driver.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"
#include "device/usbd_pvt.h"
#include "tusb.h"
#include "shizuku/objects/usb_cdc.hpp"

// ★★ここが効いているかを**ビルド時に**確かめる。tusb_config.h が届いていないと
//   CDC が 1 本のまま黙って組まれ、記述子だけ 2 本を名乗る = 列挙に失敗する
//   (実機が USB ごと消えて BOOTSEL からしか戻せなくなる)。焼く前に落とす。
static_assert(CFG_TUD_CDC == 2,
              "tusb_config.h が届いていない (CDC が 2 本になっていない)");

namespace shizuku {
namespace objects {
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
//   低優先度のユーザー IRQ を繰り返しタイマで叩く (pico_stdio_usb と同じ作法)。
uint8_t g_task_irq;

// ★診断の CDC FIFO には 2 コアと tud_task (core 0 の IRQ) が同時に触れる。
//   行単位で排他するハードウェア spinlock。tud_task は取れなければその回を
//   飛ばし (1ms 後にまた来る)、書き手は待ちを打ち切って行を捨てる (D42)。
//   どちらも無限に待たないのは、panic で止められたコアが保持したままでも
//   panic の USB 出力が詰まらないようにするため。
spin_lock_t *g_diag_lock;
void usb_task_irq_handler() {
  const uint32_t state = save_and_disable_interrupts();
  if (g_diag_lock == nullptr) {
    tud_task();
    restore_interrupts(state);
    return;
  }
  spin_lock_unsafe_blocking(g_diag_lock);
  tud_task();
  spin_unlock_unsafe(g_diag_lock);
  restore_interrupts(state);
}

int64_t usb_task_timer(alarm_id_t, void *) {
  irq_set_pending(g_task_irq);
  return 1000; // 1ms ごと
}

// ---- 診断 (printf) を channel 0 へ結ぶ --------------------------------------
// ★pico_stdio_usb を使わなくなったので、標準出力の行き先は自分で用意する。
void diag_write_unlocked(const char *buffer, int length) {
  for (int index = 0; index < length; ++index) {
    // ★CDC は 1 本ぶんのバッファしか持たない。相手が読んでいないときに詰まると
    //   系が止まるので、**溢れたら捨てる**。診断のために本業を止めない。
    if (tud_cdc_n_write_available(0) == 0)
      tud_cdc_n_write_flush(0);
    if (tud_cdc_n_write_available(0) == 0)
      return;
    tud_cdc_n_write_char(0, buffer[index]);
  }
}
void diag_out_chars(const char *buffer, int length) {
  const uint32_t state = save_and_disable_interrupts();
  if (g_diag_lock != nullptr)
    usb_cdc_diag_write_line(buffer, (uint32_t)length);
  restore_interrupts(state);
}
void diag_out_flush() { tud_cdc_n_write_flush(0); }
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

// 1 行を割込み禁止 + spinlock の下でまとめて CDC 0 へ書く。\n は \r\n にする。
void usb_cdc_diag_write_line(const char *line, uint32_t length) {
  const uint32_t state = save_and_disable_interrupts();
  if (g_diag_lock != nullptr && spin_try_lock_unsafe(g_diag_lock)) {
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    uint32_t extra_cr = 0;
    for (uint32_t index = 0; index < length; ++index)
      if (line[index] == '\n')
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
    if (available >= needed) {
      for (uint32_t index = 0; index < length; ++index) {
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
        if (line[index] == '\n')
          tud_cdc_n_write(0, "\r", 1);
#endif
      }
      tud_cdc_n_write(0, line, length);
    }
    spin_unlock_unsafe(g_diag_lock);
  }
  restore_interrupts(state);
}

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
