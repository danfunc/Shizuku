#include "shizuku/svc_attributes.hpp"
#include "shizuku/service_profile.hpp"
#include <cassert>
using namespace shizuku;
static_assert(!svc_permission_allows(23, false));
static_assert(!svc_permission_allows(25, false));
static_assert(svc_permission_allows(23, true));
static_assert(!svc_permission_allows(1, false, false, false, OBJECT_REPLACE));
static_assert(svc_permission_allows(1, true, false, false, OBJECT_REPLACE));
static_assert(!svc_permission_allows(1, true, false, false, 0x80, 0x80));
static_assert(svc_permission_allows(1, true, true, false, 0x80, 0x80));
static_assert(!svc_permission_allows(27, true, false, false));
static_assert(svc_permission_allows(27, false, false, true));
static_assert(svc_traits<object_api::YIELD>.mode == svc_mode::thread);
static_assert(!svc_traits<object_api::YIELD>.from_isr);
static_assert(svc_traits<object_api::STREAM_PUSH_N>.mode == svc_mode::handler);
int main() {
  for (uintptr_t id = 1; id <= 31; ++id) {
    const auto *a = svc_attribute_for(id);
    assert(a);
    assert(!a->from_isr || a->mode == svc_mode::handler);
    assert((a->mode == svc_mode::handler) == (a->save_level == svc_save_level::minimal));
    const bool limited = id == 23 || id == 25;
    assert((a->permission == svc_permission::privileged_only) == limited);
    if (id != 1 && id != 27) assert(svc_permission_allows(id, false) == !limited);
  }
  assert(!svc_attribute_for(0) && !svc_attribute_for(32) && !svc_attribute_for(0xDEAD));
  service_profile::maxima m;
  m.handler(0xfffffff0, 0x10); // wrap: 32, not negative
  m.handler(100, 108);
  assert(m.handler_max == 32 && m.handler_samples == 2);
  m.mask(1, 11); m.mask(20, 21);
  assert(m.mask_max == 10 && m.mask_samples == 2);
}
