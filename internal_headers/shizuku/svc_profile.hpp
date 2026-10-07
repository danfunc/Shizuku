#pragma once
#include <cstdint>
// RP2350 only. Disabled builds contain neither hooks nor storage.
#if defined(SHIZUKU_SVC_PROFILE) && SHIZUKU_SVC_PROFILE && !defined(SHIZUKU_RP2040)
#define SHIZUKU_PROFILE_ACTIVE 1
extern "C" void shizuku_svc_profile_mark(uint32_t stage);
namespace shizuku {
struct svc_profile_event { uint32_t stage, cycle; };
extern volatile svc_profile_event svc_profile_events[2][256];
extern volatile uint32_t svc_profile_count[2];
extern volatile uint32_t svc_profile_enabled[2];
extern volatile uint32_t svc_profile_mpu_cycles[2], svc_profile_mpu_updates[2], svc_profile_mpu_writes[2];
}
#define SHIZUKU_PROFILE(stage) shizuku_svc_profile_mark(stage)
#else
#define SHIZUKU_PROFILE(stage) ((void)0)
#endif
