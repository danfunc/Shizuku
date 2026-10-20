#ifndef SHIZUKU_SVC_ATTRIBUTES_HPP
#define SHIZUKU_SVC_ATTRIBUTES_HPP
#include "shizuku/object_api.hpp"
#include <array>

namespace shizuku {
enum class svc_permission { unprivileged_allowed, privileged_only };
enum class svc_mode { handler, thread };
enum class svc_save_level { minimal, continuation };
enum class svc_authority { none, create_flags, parent_ledger };
struct svc_attribute {
  svc_permission permission;
  bool from_isr;
  svc_mode mode;
  svc_save_level save_level;
  svc_authority authority;
};
// In V3 Stage V1, when an SVC resolves to the root KernelObject handler (to_root == true),
// svc_mode::handler selects direct kernel trap handling for synchronous bounded APIs
// (GET_CURRENT_OBJECT / GET_CALLER_OBJECT) without pushing a dispatch frame.
// Mode does NOT alter objectland raw routing: if a custom dedicated parent handler is
// bound (to_root == false), raw SVC delivery to the registered parent is strictly preserved.
inline constexpr auto svc_attributes = [] {
  std::array<svc_attribute, 32> a{};
  for (auto &v : a)
    v = {svc_permission::unprivileged_allowed, false, svc_mode::thread,
         svc_save_level::continuation, svc_authority::none};
  for (unsigned i : {5u, 6u, 16u, 18u, 20u, 29u, 30u, 31u}) {
    a[i].mode = svc_mode::handler;
    a[i].save_level = svc_save_level::minimal;
  }
  for (unsigned i : {18u, 20u, 29u, 30u, 31u}) a[i].from_isr = true;
  for (unsigned i : {23u, 25u})
    a[i].permission = svc_permission::privileged_only;
  a[1].authority = svc_authority::create_flags;
  // V2 authorizes this by protected parent/child ledger, not merely privilege.
  a[27].authority = svc_authority::parent_ledger;
  return a;
}();
constexpr const svc_attribute *svc_attribute_for(uintptr_t id) {
  return id > 0 && id < svc_attributes.size() ? &svc_attributes[id] : nullptr;
}
constexpr bool svc_permission_allows(uintptr_t id, bool privileged,
                                      bool composition = false,
                                      bool parent = false, uintptr_t flags = 0,
                                      uintptr_t handler_flag = 0) {
  const auto *a = svc_attribute_for(id);
  if (!a) return false;
  if (a->permission == svc_permission::privileged_only && !privileged) return false;
  if (a->authority == svc_authority::parent_ledger) return parent;
  if (a->authority == svc_authority::create_flags) {
    if ((flags & OBJECT_REPLACE) && !privileged) return false;
    if ((flags & handler_flag) && !composition) return false;
  }
  return true;
}
template<object_api Api> inline constexpr auto svc_traits =
    svc_attributes[static_cast<uintptr_t>(Api)];
static_assert(svc_traits<object_api::GRANT_REGION>.permission == svc_permission::privileged_only);
static_assert(svc_traits<object_api::KILL_THREAD>.permission == svc_permission::privileged_only);
static_assert(svc_traits<object_api::FORWARD_CHILD_EXIT>.authority == svc_authority::parent_ledger);
} // namespace shizuku
#endif
