#ifndef SHIZUKU_TEMPLATES_TASK_HPP
#define SHIZUKU_TEMPLATES_TASK_HPP
#include <cstdint>

namespace shizuku::templates {

// CPU-owned execution-right history, not a thread or an execution context.
// One instance belongs to each (core, execution-priority) pair. Only its owner
// may mutate it, with grant/timer operations serialized by the port.
// Never retain a pointer into a lender's stack: it may be destroyed first.
struct task {
  struct grant_frame {
    uint32_t lender;
    uint32_t generation;
    uint64_t remaining;
  };
  static constexpr uint32_t MAX_DEPTH = 8;
  grant_frame frames[MAX_DEPTH]{};
  uint32_t depth = 0;
  // Request and acknowledgement are distinct. A remote request alone is not
  // proof that the owning core has stopped changing this history.
  uint32_t move_blocked = 0;
  uint32_t paused = 0;
  uint32_t armed = 0; // uncharged part of the hardware timer interval, in cycles

  void charge(uint64_t cycles) {
    for (uint32_t i = 0; i < depth; ++i)
      frames[i].remaining = frames[i].remaining > cycles
                                ? frames[i].remaining - cycles : 0;
  }
};

// Priority is an execution lane, not a scheduling weight or the IRQ number of
// SVC/PendSV. Those handlers service the interrupted lane's task.
template <uintptr_t Cores, uintptr_t Priorities> struct task_table {
  static_assert(Cores > 0 && Priorities > 0);
  task entries[Cores][Priorities]{};
  task &at(uint32_t core, uint32_t priority) { return entries[core][priority]; }
  const task &at(uint32_t core, uint32_t priority) const {
    return entries[core][priority];
  }
};
} // namespace shizuku::templates
#endif
