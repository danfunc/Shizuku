#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <atomic>
#include <cstdio>
#define private public
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#undef private
using namespace shizuku;
using State = KERNEL::THREAD::state_t;
static TestArch::exception_frame_t frames[16];
static bool hook_ready = true;
static bool hook(uint32_t) { return hook_ready; }
static uintptr_t method(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
static void reset() {
  TestBoard::core = 0;
  TestBoard::notify = nullptr;
  TestBoard::dma_active = false;
  TestArch::remaining = 0;
  kernel_instance.init();
  kernel_object_instance.init();
  auto &k = kernel_instance;
  k.m_current[0] = 0;
  k.m_online[0] = 1;
  k.m_threads[0].thread.set_state(State::RUNNING);
  for (unsigned t = 0; t < 16; ++t) {
    frames[t] = {};
    k.m_threads[t].context.sp = &frames[t];
  }
  k.task_attach(0, 0);
  hook_ready = true;
}
static void create(uint32_t obj) {
  object_error err{};
  assert(kernel_object_instance.create_object(obj, (uintptr_t)method,
                                               OBJECT_UNPRIVILEGED, err) == obj);
  assert(err == object_error::OK);
  kernel_object_instance.set_destroy_hook(obj, hook);
}
static void worker() {
  auto &k = kernel_instance;
  auto &o = kernel_object_instance;
  k.m_threads[k.m_current[0]].thread.set_state(State::READY);
  const auto old = k.m_current[0];
  k.m_current[0] = o.m_destroy_worker;
  k.task_detach(old, 0);
  k.task_attach(o.m_destroy_worker, 0);
  k.m_threads[o.m_destroy_worker].thread.set_state(State::RUNNING);
}
int main() {
  reset(); create(2);
  auto &k = kernel_instance;
  auto &o = kernel_object_instance;
  object_error err{};
  // No implicit destruction of firmware objects that have external callbacks.
  o.set_destroy_hook(2, nullptr);
  assert(o.destroy_object(2, err) == 0 && err == object_error::DESTROY_BUSY);
  o.set_destroy_hook(2, hook); err = object_error::OK;
  const auto t = o.spawn_method(2, 0, 0, err);
  assert(err == object_error::OK && k.thread_state(t) == State::READY);
  const auto allocation = o.arena_allocate(o.m_objects_arena, 64, 2);
  assert(allocation);
  const auto ticket = o.destroy_object(2, err);
  assert(ticket && err == object_error::OK);
  o.spawn_method(2, 0, 0, err);
  assert(err == object_error::BAD_OBJECT);
  worker(); hook_ready = false;
  assert(!o.destroy_poll());
  assert(o.m_objects[2].created && o.m_thread_stack[t]);
  assert(k.thread_state(t) == State::TERMINATED);
  o.schedule(o.m_destroy_worker);
  assert(o.m_thread_stack[t]); // deferred retirement remains pinned
  hook_ready = true;
  assert(o.destroy_poll());
  assert(!o.m_objects[2].created && o.m_thread_stack[t] == 0);
  assert(k.thread_state(t) == State::UNINITIALIZED);
  assert(o.destroy_status(ticket, err) == 1);

  // A call into the dying object invalidates the WHOLE calling thread, even
  // when that thread was spawned by a different object.
  reset(); create(2); create(3); err = object_error::OK;
  const auto caller = o.spawn_method(3, 0, 0, err);
  o.m_shadow[caller].depth = 1;
  o.m_shadow[caller].object[0] = 2;
  o.m_shadow[caller].caller[0] = 3;
  assert(o.destroy_object(2, err)); worker();
  assert(o.destroy_poll());
  assert(k.thread_state(caller) == State::UNINITIALIZED);
  assert(o.m_objects[3].created);

  // Remote active borrower: real concurrent park/ack/resume; return to its
  // surviving lender, then reclaim the borrower's stack on the coordinator.
  reset(); create(2); create(3); err = object_error::OK;
  const auto borrower = o.spawn_method(2, 0, 0, err);
  const auto lender = o.spawn_method(3, 0, 0, err);
  assert(o.destroy_object(2, err)); worker();
  k.m_online[1] = 1;
  k.m_current[1] = borrower;
  k.m_threads[borrower].thread.set_state(State::RUNNING);
  k.m_threads[lender].thread.set_state(State::WAIT_GRANT);
  auto &remote = k.cpu_manager.execution_task(1);
  remote.depth = 1;
  remote.frames[0] = {(uint32_t)lender, k.m_threads[lender].thread.generation, 100};
  remote.armed = 100;
  k.task_attach(borrower, 1); k.task_attach(lender, 1);
  std::atomic<bool> go{false};
  std::thread other([&] {
    TestBoard::core = 1; TestArch::remaining = 90;
    while (!go.load()) {}
    while (!TestArch::load_acquire32(&remote.move_blocked)) {}
    assert(k.pause_dispatch());
    assert(k.m_current[1] == lender);
    assert(remote.depth == 0);
  });
  go.store(true);
  // A loaded test machine may miss a 1ms pause window. Retry as the worker does.
  while (!o.destroy_poll()) {}
  other.join();
  assert(k.thread_state(borrower) == State::UNINITIALIZED);
  assert(k.thread_tasks(lender) == 2 && k.thread_tasks(borrower) == 0);
  assert(k.m_threads[lender].context.sp->slot[1] == (uintptr_t)grant_end::EXPIRED);

  // Self-destroy: requester is the local GRANT lender, so it must disappear
  // from the worker's history before its stack is returned to the arena.
  reset(); create(2); err = object_error::OK;
  const auto self = o.spawn_method(2, 0, 0, err);
  assert(o.destroy_object(2, err));
  k.m_threads[0].thread.set_state(State::READY);
  k.m_current[0] = self;
  k.task_detach(0, 0); k.task_attach(self, 0);
  k.m_threads[self].thread.set_state(State::RUNNING);
  assert(k.do_grant(o.m_destroy_worker, 100) == kernel_error::OK);
  assert(o.destroy_poll());
  assert(k.cpu_manager.execution_task(0).depth == 0);
  assert(k.thread_tasks(self) == 0 && k.thread_state(self) == State::UNINITIALIZED);
  assert(k.do_switch(o.m_destroy_fallback[0]) == kernel_error::OK);
  assert(k.current_thread_id() == o.m_destroy_fallback[0]);

  // Timeout must not publish completion, invalidate a context, or leave flags.
  reset(); create(2); err = object_error::OK;
  const auto live = o.spawn_method(2, 0, 0, err);
  assert(o.destroy_object(2, err)); worker();
  k.m_online[1] = 1; // no remote handler to acknowledge
  assert(!o.destroy_poll());
  assert(k.thread_state(live) == State::READY);
  assert(k.m_pause_owner == 0 && k.cpu_manager.execution_task(1).move_blocked == 0);

  // DMA completion is a prerequisite, not something CPU park implies.
  reset(); create(2); err = object_error::OK;
  stream::descriptor from{}, to{};
  from.producer = 2; from.consumer = stream::CONNECTED;
  to.producer = stream::CONNECTED; to.consumer = 3;
  o.m_streams[0] = &from; o.m_stream_owner[0] = 2;
  o.m_streams[1] = &to; o.m_stream_owner[1] = 1;
  o.m_connections[0] = {o.CONNECTION_ACTIVE, 0, 1, 0, 1, 0, 0, 0};
  o.m_connection_count = 1;
  assert(o.destroy_object(2, err)); worker();
  TestBoard::dma_active = true;
  assert(!o.destroy_poll() && o.m_streams[0] == &from);
  TestBoard::dma_active = false;
  assert(o.destroy_poll() && o.m_streams[0] == nullptr);
  assert(o.m_connection_count == 0 && to.producer == stream::NO_OWNER);
  // One thread may still be registered in several histories (e.g. a suspended
  // lender subsequently resumed on another core). Prune every registered task.
  reset();
  k.m_pause_owner = 1; k.m_pause_epoch = 7; k.m_online[1] = 1;
  k.m_current[1] = 3;
  k.m_threads[2].thread.set_state(State::WAIT_GRANT);
  k.m_threads[3].thread.set_state(State::RUNNING);
  auto &local_task = k.cpu_manager.execution_task(0);
  auto &remote_task = k.cpu_manager.execution_task(1);
  local_task.move_blocked = remote_task.move_blocked = remote_task.paused = 7;
  local_task.depth = remote_task.depth = 1;
  local_task.frames[0] = remote_task.frames[0] = {2, 0, 100};
  k.task_attach(2, 0); k.task_attach(2, 1); k.task_attach(3, 1);
  uint32_t fallbacks[2] = {4, 5};
  assert(k.destroy_contexts(1u << 2, fallbacks));
  assert(!local_task.depth && !remote_task.depth && !k.thread_tasks(2));

  // Active remote victim with no surviving lender uses the policy-provided
  // fallback. A suspended lender receives a result but remains suspended.
  remote_task.depth = 1;
  remote_task.frames[0] = {4, 0, 100};
  k.m_threads[4].thread.set_state(State::SUSPENDED);
  k.m_threads[5].thread.set_state(State::READY);
  k.m_threads[5].thread.affinity = 2;
  k.task_attach(4, 1);
  assert(k.destroy_contexts(1u << 3, fallbacks));
  assert(k.m_current[1] == 5 && k.thread_state(4) == State::SUSPENDED);
  assert(k.thread_tasks(4) == 0 && k.thread_tasks(5) == 2);

  // A cancelled request's acknowledgement is never accepted for a new epoch.
  reset(); k.m_online[1] = 1; k.m_pause_epoch = 41;
  k.cpu_manager.execution_task(1).paused = 41;
  assert(!k.pause_tasks(100));
  assert(k.m_pause_owner == 0);
  puts("object destroy regression tests passed");
}
