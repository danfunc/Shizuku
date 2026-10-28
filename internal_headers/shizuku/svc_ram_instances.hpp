#ifndef SHIZUKU_SVC_RAM_INSTANCES_HPP
#define SHIZUKU_SVC_RAM_INSTANCES_HPP
#include "shizuku/hot.hpp"
// Include after config aliases. GCC 13 (PR70435) does not propagate a section
// attribute from the primary template: extern instances suppress flash copies,
// while one glue TU defines them. Inlining remains enabled. Each instance uses
// its own section because COMDAT groups cannot share one output input section.
#if defined(SHIZUKU_SVC_IN_RAM) && SHIZUKU_SVC_IN_RAM && defined(__ARM_ARCH_8M_MAIN__)
#ifdef SHIZUKU_SVC_RAM_DEFINE_INSTANCES
#define SHIZUKU_SVC_EXTERN
#else
#define SHIZUKU_SVC_EXTERN extern
#endif
#define SHIZUKU_SVC_INSTANCE(name) __attribute__((section(".time_critical.shizuku." #name)))
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(arg) uintptr_t shizuku::ARCH::arg(const shizuku::ARCH::exception_frame_t &, unsigned);
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(stack) void shizuku::ARCH::apply_stack_window(uint32_t, uint32_t);
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(window) void shizuku::ARCH::ensure_object_window(uint32_t);
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(object_window) void shizuku::ARCH::apply_object_window(const shizuku::ARCH::object_protection_window &);
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(return_room) bool shizuku::ARCH::call_entry_has_return_room(const shizuku::ARCH::context_t &, uint32_t);
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(floor) uint32_t shizuku::KERNEL::current_unwind_floor() const;
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(find_desc) const shizuku::KERNEL_OBJECT::method_descriptor_entry *shizuku::KERNEL_OBJECT::find_method_descriptor(uint32_t, uint32_t, uint32_t, shizuku::object_error &) const;
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(request) shizuku::object_error shizuku::KERNEL_OBJECT::method_call_request(uintptr_t, uintptr_t, uintptr_t, shizuku::call_request &) const;
SHIZUKU_SVC_EXTERN template SHIZUKU_SVC_INSTANCE(can_call) bool shizuku::KERNEL_OBJECT::can_call_method(uintptr_t, uintptr_t) const;
#undef SHIZUKU_SVC_INSTANCE
#undef SHIZUKU_SVC_EXTERN
#endif
#endif
