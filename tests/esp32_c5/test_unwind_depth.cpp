#include "shizuku/kernel_abi.hpp"
#include "arch/armv6m/armv6m.hpp"

#include <cassert>

using shizuku::kernel_error;
using shizuku::resolve_return_stub;
using shizuku::validate_return_depth;
using armv6m = shizuku::archs::armv6m<8, 2>;

int main() {
  // One-frame return to the actor's floor.
  assert(validate_return_depth(4, 3, 3, 1) == kernel_error::OK);
  // Two frames may be folded when both are above the same floor.
  assert(validate_return_depth(5, 3, 3, 2) == kernel_error::OK);
  // Neither an ancestor outside the actor's scope nor the current depth is a
  // valid destination.
  assert(validate_return_depth(5, 3, 2, 3) == kernel_error::DEPTH_MISMATCH);
  assert(validate_return_depth(5, 3, 5, 0) == kernel_error::DEPTH_MISMATCH);
  // A valid destination still requires an exact count derived from the depth.
  assert(validate_return_depth(5, 3, 3, 1) == kernel_error::BAD_COUNT);
  assert(validate_return_depth(5, 3, 4, 0) == kernel_error::BAD_COUNT);

  // A KOBJ return stub pops its own dispatch frame to the immediate caller;
  // multi-frame exits use the explicit target-depth API above.
  const auto one_frame_stub = resolve_return_stub(4);
  assert(one_frame_stub.target_depth == 3 && one_frame_stub.count == 1);
  const auto nested_stub = resolve_return_stub(5);
  assert(nested_stub.target_depth == 4 && nested_stub.count == 1);
  const auto empty_stub = resolve_return_stub(0);
  assert(empty_stub.target_depth == 0 && empty_stub.count == 0);

  // ARMv6-M uses AAPCS: arguments 1-4 are in r0-r3, argument 5 starts at SP.
  assert(armv6m::HANDLER_STACK_ARG_BYTES == 8);
  armv6m::exception_frame_t frame{};
  const uintptr_t args[5] = {1, 2, 3, 4, 0x12345678};
  alignas(8) uint32_t stack_args[2] = {0, 0xA5A5A5A5};
  armv6m::set_handler_args(frame, args,
                           reinterpret_cast<uintptr_t>(stack_args));
  assert(frame.r0 == 1 && frame.r1 == 2 && frame.r2 == 3 && frame.r3 == 4);
  assert(stack_args[0] == 0x12345678);
  assert(stack_args[1] == 0xA5A5A5A5);

  // RETURN replies use r0=error and r1=value; the saved user argument lanes
  // must not become a return error while the kernel updates those outputs.
  frame.r2 = 0x22222222;
  frame.r3 = 0;
  frame.r12 = 0xA55A;
  armv6m::set_result(frame, 0, 0x12345678);
  assert(frame.r0 == 0 && frame.r1 == 0x12345678);
  assert(frame.r2 == 0x22222222 && frame.r3 == 0 && frame.r12 == 0xA55A);
}
