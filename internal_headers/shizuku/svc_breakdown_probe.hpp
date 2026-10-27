#pragma once
#include <cstddef>
#include <cstdint>

namespace shizuku::selftest {

inline constexpr size_t SVC_BREAKDOWN_POINT_COUNT = 7;
inline constexpr uint32_t SVC_BREAKDOWN_API_GET_CURRENT_OBJECT = 5;

struct svc_breakdown_probe_state_t {
  bool armed = false;
  bool active = false;
  bool invalid = false;
  uint32_t next_point = 0;
  uint32_t completed = 0;
  uint32_t session = 0;
  uint32_t active_session = 0;
  uint32_t hits[SVC_BREAKDOWN_POINT_COUNT] = {};
  uint32_t timestamps[SVC_BREAKDOWN_POINT_COUNT] = {};
  uintptr_t owner = 0;
  uintptr_t context = 0;
  uintptr_t expected_owner = 0;
  uintptr_t expected_context = 0;
  uint32_t exc_return = 0;
  uint32_t psp = 0;
};

inline void arm_svc_breakdown_probe(svc_breakdown_probe_state_t &state,
                                    uintptr_t expected_owner,
                                    uintptr_t expected_context) {
  state.armed = true;
  state.active = false;
  state.next_point = 0;
  state.expected_owner = expected_owner;
  state.expected_context = expected_context;
  ++state.session;
  if (expected_owner == 0 || expected_context == 0) state.invalid = true;
}

inline void disarm_svc_breakdown_probe(svc_breakdown_probe_state_t &state) {
  state.armed = false;
  state.active = false;
}

inline bool valid_thread_psp_exc_return(uint32_t value) {
  return (value & 0xFFFFFFE1u) == 0xFFFFFFE1u && (value & 0xCu) == 0xCu;
}

inline bool timestamp_delta_valid(uint32_t earlier, uint32_t later) {
  const uint32_t delta = later - earlier;
  return delta != 0 && delta < 0x80000000u;
}

inline void observe_svc_breakdown_probe(svc_breakdown_probe_state_t &state,
                                        uint32_t point, uint32_t api_number,
                                        uintptr_t owner, uintptr_t context,
                                        uint32_t exc_return, uint32_t psp,
                                        uint32_t timestamp) {
  if (point == 0) {
    if (!state.armed) return;
    if (api_number != SVC_BREAKDOWN_API_GET_CURRENT_OBJECT) return;
    if (owner != state.expected_owner || context != state.expected_context) return;
    state.armed = false;
    if (!valid_thread_psp_exc_return(exc_return) || psp == 0 ||
        (psp & 3u) != 0) {
      state.invalid = true;
      return;
    }
    state.active = true;
    state.active_session = state.session;
    state.next_point = 1;
    state.owner = owner;
    state.context = context;
    state.exc_return = exc_return;
    state.psp = psp;
    state.timestamps[0] = timestamp;
    ++state.hits[0];
    return;
  }
  if (!state.active) return;
  if (point >= SVC_BREAKDOWN_POINT_COUNT || point != state.next_point ||
      owner != state.owner || context != state.context ||
      !timestamp_delta_valid(state.timestamps[point - 1], timestamp) ||
      (point + 1 == SVC_BREAKDOWN_POINT_COUNT && psp != state.psp)) {
    state.invalid = true;
    state.active = false;
    return;
  }
  state.timestamps[point] = timestamp;
  ++state.hits[point];
  ++state.next_point;
  if (point + 1 == SVC_BREAKDOWN_POINT_COUNT) {
    state.active = false;
    ++state.completed;
  }
}

inline bool svc_breakdown_sample_ok(const svc_breakdown_probe_state_t &state,
                                    uint32_t expected_completed) {
  if (state.invalid || state.armed || state.active ||
      state.completed != expected_completed || state.session != expected_completed ||
      state.active_session != expected_completed)
    return false;
  for (size_t i = 0; i < SVC_BREAKDOWN_POINT_COUNT; ++i)
    if (state.hits[i] != expected_completed) return false;
  return true;
}

} // namespace shizuku::selftest
