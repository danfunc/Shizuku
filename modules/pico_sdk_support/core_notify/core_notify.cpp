#include "core_notify.hpp"
#include "hardware/timer.h"
#include "pico/platform.h"
#include "pico/multicore.h"
namespace shizuku::ports {
static int alarms[2] = {-1, -1};
static void (*callbacks[2])() = {};
static void notify_irq(unsigned) {
  if (auto callback = callbacks[get_core_num()])
    callback();
}
void core_notify_init(uint32_t core, void (*callback)()) {
  const int alarm = hardware_alarm_claim_unused(true);
  callbacks[core] = callback;
  hardware_alarm_set_callback((unsigned)alarm, notify_irq);
  __atomic_store_n(&alarms[core], alarm, __ATOMIC_RELEASE);
}
void core_notify(uint32_t core) {
  const int alarm = __atomic_load_n(&alarms[core], __ATOMIC_ACQUIRE);
  if (alarm >= 0)
    hardware_alarm_force_irq((unsigned)alarm);
}
}
