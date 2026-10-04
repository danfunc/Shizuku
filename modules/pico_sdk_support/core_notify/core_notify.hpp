#pragma once
#include <cstdint>
namespace shizuku::ports {
// Port mechanism only. Callback must request deferred kernel work, not destroy
// objects or inspect object/thread tables. Each core owns one hardware alarm.
void core_notify_init(uint32_t core, void (*callback)());
void core_notify(uint32_t core);
}
