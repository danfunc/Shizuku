#include "chips/esp32_c6/stop_capture.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace capture = shizuku::chips::esp32_c6::stop_capture;

struct test_context {
  uint32_t sp;
  uint8_t privileged;
  uint8_t padding[3];
};

static_assert(capture::layout_of<test_context>().object_bytes == sizeof(test_context),
              "layout_of object_bytes must match sizeof");
static_assert(capture::layout_of<test_context>().sp_offset == offsetof(test_context, sp),
              "layout_of sp_offset must match offsetof");
static_assert(capture::layout_of<test_context>().privileged_offset ==
                  offsetof(test_context, privileged),
              "layout_of privileged_offset must match offsetof");

int main() {
  alignas(16) uint32_t frame[capture::kFrameWords]{};
  for (uint32_t i = 0; i < capture::kFrameWords; ++i) frame[i] = 0x1000u + i;
  test_context context{0x40831230u, 1u, {0, 0, 0}};
  const uintptr_t frame_address = reinterpret_cast<uintptr_t>(frame);
  const uintptr_t context_address = reinterpret_cast<uintptr_t>(&context);
  const capture::ram_range frame_range{frame_address,
                                       frame_address + sizeof(frame)};
  const capture::ram_range context_range{
      context_address, context_address + sizeof(context)};
  const capture::context_layout layout{sizeof(context), offsetof(test_context, sp),
                                       offsetof(test_context, privileged)};
  capture::capture_record record{};

  capture::capture_input input{0xE002u, 7u, 0x42001234u, frame_address,
                               context_address, 11u, 2u, 0u, 0x1880u,
                               0x40831200u, 0x42005678u, 1u, 0u, 0xC602u, 7u};
  const uint32_t valid = capture::capture(record, input, frame_range,
                                           context_range, layout);
  assert(record.magic == capture::kMagic && record.version == capture::kVersion);
  assert(valid == (capture::kFrameValid | capture::kContextValid));
  assert(record.frame_address == frame_address);
  assert(record.context_address == context_address);
  assert(record.frame_words[0] == 0x1000u);
  assert(record.frame_words[35] == 0x1023u);
  assert(record.context_sp == 0x40831230u && record.context_privileged == 1u);
  assert(record.status == 0xE002u && record.phase == 7u);
  assert(record.caller_pc == 0x42001234u && record.mcause == 11u);
  assert(record.mepc == 2u && record.mstatus == 0x1880u);
  assert(record.sp == 0x40831200u && record.ra == 0x42005678u);
  assert(record.trap_depth == 1u && record.irq_depth == 0u);
  assert(record.boot_marker == 0xC602u);
  assert(record.boot_generation == 7u);
  std::puts("PASS valid 144-byte frame and bounded context copy");

  input.frame_address = 0x20000000u;
  input.context_address = 0x20000000u;
  input.boot_marker = 0u;
  input.boot_generation = 0u;
  const capture::ram_range c6_ram{0x40800000u, 0x40860000u};
  volatile capture::capture_record durable_record{};
  const uint32_t bad_address_valid =
      capture::capture(durable_record, input, c6_ram, c6_ram, layout);
  assert(durable_record.magic == capture::kMagic);
  assert(durable_record.frame_address == 0x20000000u);
  assert(durable_record.context_address == 0x20000000u);
  assert(durable_record.boot_marker == 0u);
  assert(durable_record.boot_generation == 0u);
  assert(bad_address_valid == 0u);
  assert(durable_record.frame_words[0] == 0u &&
         durable_record.frame_words[35] == 0u);
  assert(durable_record.context_sp == 0u &&
         durable_record.context_privileged == 0u);
  std::puts("PASS MMIO-like bad pointers retained without dereference");

  input.frame_address = frame_address + 1u;
  input.context_address = context_address;
  const uint32_t unaligned_valid = capture::capture(
      record, input, frame_range, context_range, layout);
  assert((unaligned_valid & capture::kFrameValid) == 0u);
  assert((unaligned_valid & capture::kContextValid) != 0u);
  assert(record.frame_address == frame_address + 1u);
  std::puts("PASS unaligned frame rejected while valid context is copied");

  const uintptr_t near_wrap = UINTPTR_MAX - 0x8fu;
  const capture::ram_range overflow_range{near_wrap, UINTPTR_MAX};
  assert(!capture::range_contains(overflow_range, near_wrap,
                                  capture::kFrameBytes, alignof(uint32_t)));
  assert(!capture::range_contains(frame_range, frame_address,
                                  sizeof(frame) + sizeof(uint32_t),
                                  alignof(uint32_t)));
  assert(!capture::range_contains({frame_range.end, frame_range.begin},
                                  frame_address, sizeof(uint32_t),
                                  alignof(uint32_t)));
  input.frame_address = near_wrap;
  input.context_address = 0x20000000u;
  const uint32_t overflow_capture_valid = capture::capture(
      record, input, overflow_range, c6_ram, layout);
  assert(record.magic == capture::kMagic);
  assert(record.frame_address == near_wrap);
  assert(overflow_capture_valid == 0u && record.frame_words[0] == 0u);
  std::puts("PASS overflow, end-of-range, and invalid-range guards");

  // Verify compile-time layout deduction via capture<Context>(...).
  input.frame_address = frame_address;
  input.context_address = context_address;
  input.boot_marker = 0xC602u;
  input.boot_generation = 7u;
  capture::capture_record deduced_record{};
  const uint32_t deduced_valid = capture::capture<test_context>(
      deduced_record, input, frame_range, context_range);
  assert(deduced_valid == (capture::kFrameValid | capture::kContextValid));
  assert(deduced_record.magic == capture::kMagic);
  assert(deduced_record.context_sp == 0x40831230u);
  assert(deduced_record.context_privileged == 1u);
  assert(capture::is_valid(deduced_record));
  assert(capture::validated_record(deduced_record) == &deduced_record);
  std::puts("PASS capture<test_context> compile-time layout deduction");

  // Verify retained_slot and magic guards.
  capture::retained_slot<capture::capture_record> slot{};
  assert(!slot.is_valid());
  assert(slot.get_if_valid() == nullptr);
  capture::capture<test_context>(slot.storage, input, frame_range, context_range);
  assert(slot.is_valid());
  assert(slot.get_if_valid() != nullptr);
  assert(slot.get_if_valid()->context_sp == 0x40831230u);
  slot.invalidate();
  assert(!slot.is_valid());
  assert(slot.get_if_valid() == nullptr);
  std::puts("PASS retained_slot magic checking and invalidation");

  return 0;
}
