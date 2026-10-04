#include <chrono>
#include <cstring>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
// White-box harness calls the real kernel functions with a host arch. No test
// hooks or public mutation APIs are added to the firmware.
#define private public
#include "shizuku/kernel.hpp"
#undef private
#include "../../source/cpu_manager/init.cpp"
#include "../../source/kernel/destroy.cpp"
#include "../../source/kernel/init.cpp"
#include "../../source/kernel/thread.cpp"
#include "../../source/kernel/dispatch.cpp"

using namespace shizuku;
using State = KERNEL::THREAD::state_t;
KERNEL::thread_record records[8];
TestArch::exception_frame_t frames[8];

void reset() {
  TestBoard::core = 0;
  TestArch::remaining = 0;
  TestArch::pending = false;
  kernel_instance.init();
  kernel_instance.set_thread_storage(records, sizeof(records));
  for (unsigned i = 0; i < 8; ++i) {
    frames[i] = {};
    records[i].context.sp = &frames[i];
    records[i].thread.affinity = 3;
    records[i].thread.set_state(State::READY);
  }
  records[0].thread.set_state(State::RUNNING);
  kernel_instance.set_recovery_thread(7);
}

int main() {
  // Separate history and timer bookkeeping for every core/priority pair.
  templates::cpu_manager<TestArch, TestBoard, 2, 3> cpu;
  cpu.execution_task(0, 1).depth = 1;
  cpu.execution_task(0, 1).armed = 91;
  assert(cpu.execution_task(0, 0).depth == 0);
  assert(cpu.execution_task(1, 1).armed == 0);
  assert(cpu.execution_task(0, 2).depth == 0);
  cpu.reset_tasks();
  assert(cpu.execution_task(0, 1).depth == 0);
  assert(cpu.execution_task(0, 1).armed == 0);

  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  TestArch::remaining = 80;
  assert(kernel_instance.do_grant(2, 100) == kernel_error::OK);
  auto &task = kernel_instance.cpu_manager.execution_task(0);
  assert(task.depth == 2 && task.frames[1].remaining == 80);
  TestArch::remaining = 70;
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 1);
  assert(task.depth == 1 && task.frames[0].remaining == 70);
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 0 && task.depth == 0);
  assert(task.armed == 0 && TestArch::remaining == 0);

  // Destroyed intermediate lender: poison its stack pointer. Unwind must
  // skip it without touching that pointer, and return to the outer lender.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  assert(kernel_instance.do_grant(2, 80) == kernel_error::OK);
  kernel_instance.terminate(1);
  records[1].context.sp = nullptr;
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 0);
  assert(records[1].thread.is_state(State::TERMINATED));
  assert(records[2].thread.is_state(State::READY));

  // Reused ID must not receive the previous generation's return value.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  assert(kernel_instance.do_grant(2, 80) == kernel_error::OK);
  kernel_instance.terminate(1);
  kernel_instance.release(1);
  records[1].thread.set_state(State::WAIT_GRANT);
  records[1].context.sp = nullptr;
  kernel_instance.grant_unwind(grant_end::EXPIRED);
  assert(kernel_instance.current_thread_id() == 0);
  assert(records[1].thread.is_state(State::WAIT_GRANT));

  // Debugger suspension must not become the exception-return target.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  kernel_instance.suspend(0);
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 7);
  assert(records[0].thread.is_state(State::SUSPENDED));
  assert(frames[0].slot[1] == (uintptr_t)grant_end::YIELDED);

  // A terminating borrower and no surviving lender need a recovery context.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  kernel_instance.terminate(0);
  records[0].context.sp = nullptr;
  kernel_instance.terminate(1);
  kernel_instance.grant_unwind(grant_end::EXPIRED);
  assert(kernel_instance.current_thread_id() == 7);
  assert(records[1].thread.is_state(State::TERMINATED));

  // Refuse a too-small nested budget without claiming the candidate.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  TestArch::remaining = 5;
  assert(kernel_instance.do_grant(2, 100) == kernel_error::GRANT_TOO_SMALL);
  assert(records[2].thread.is_state(State::READY));

  // Exercise actual timer/PendSV return, not just direct unwind calls.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  TestArch::remaining = 0;
  kernel_instance.timer_expired();
  assert(TestArch::pending);
  kernel_instance.pendsv_dispatch(&records[1].context);
  assert(kernel_instance.current_thread_id() == 0);
  assert(frames[0].slot[1] == (uintptr_t)grant_end::EXPIRED);

  // An orphaned but live borrower may keep executing if recovery is busy.
  reset();
  assert(kernel_instance.do_grant(1, 100) == kernel_error::OK);
  kernel_instance.terminate(0);
  records[0].context.sp = nullptr;
  records[7].thread.set_state(State::RUNNING);
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 1);
  assert(records[1].thread.is_state(State::RUNNING));
  assert(!kernel_instance.grant_active());

  // Exercise the second core through the production grant path.
  reset();
  TestBoard::core = 1;
  kernel_instance.m_current[1] = 3;
  records[3].thread.set_state(State::RUNNING);
  assert(kernel_instance.do_grant(4, 90) == kernel_error::OK);
  assert(kernel_instance.cpu_manager.execution_task(0).depth == 0);
  assert(kernel_instance.cpu_manager.execution_task(1).depth == 1);
  kernel_instance.grant_unwind(grant_end::YIELDED);
  assert(kernel_instance.current_thread_id() == 3);
  std::puts("task grant regression tests passed");
}
