#include "shizuku/region_capability.hpp"
#include "chips/esp32_c6/pmp_c6.hpp"

#include <array>
#include <cassert>
#include <cstdint>

using namespace shizuku::region_capability;
using namespace shizuku::chips::esp32_c6;

static_assert(static_cast<uint8_t>(ObjectRole::Root) == static_cast<uint8_t>(c6_stage1_role::Root),
              "ObjectRole::Root must match c6_stage1_role::Root");
static_assert(static_cast<uint8_t>(ObjectRole::App) == static_cast<uint8_t>(c6_stage1_role::App),
              "ObjectRole::App must match c6_stage1_role::App");
static_assert(static_cast<uint8_t>(ObjectRole::Driver) == static_cast<uint8_t>(c6_stage1_role::Driver),
              "ObjectRole::Driver must match c6_stage1_role::Driver");

constexpr uint32_t ROOT = 1;
constexpr uint32_t APP = 2;
constexpr uint32_t DRIVER = 3;
constexpr uint32_t ROOT_GEN = 7;
constexpr uint32_t APP_GEN = 11;
constexpr uint32_t DRIVER_GEN = 13;

constexpr RegionCapability root_source(uint32_t base, uint32_t length,
                                       uint32_t rights, RegionKind kind) {
  return {base, length, rights, kind, ROOT, ROOT, ROOT_GEN, ROOT_GEN};
}

int main() {
  RegionCapability cap{};
  auto app_private = root_source(0x40800000u, 0x1000u, kRead | kExecute,
                                 RegionKind::AppPrivate);
  GrantRequest to_app{ROOT, APP, ObjectRole::App, 0x40800000u, 0x1000u,
                      kRead | kExecute, RegionKind::AppPrivate};
  assert(make_grant(ROOT, ROOT_GEN, APP_GEN, app_private, to_app, &cap) == Error::Ok);
  assert(cap.issuer_object == ROOT && cap.target_object == APP);
  assert(cap.target_generation == APP_GEN);
  assert(is_live(cap, ROOT, ROOT_GEN, APP, APP_GEN));
  assert(!is_live(cap, ROOT, ROOT_GEN, APP, APP_GEN + 1)); // revoke makes stale

  // App/driver private memory and MMIO remain role-separated.
  GrantRequest app_gets_mmio{ROOT, APP, ObjectRole::App, 0x60000000u, 0x1000u,
                             kRead | kWrite, RegionKind::DriverMmio};
  assert(make_grant(ROOT, ROOT_GEN, APP_GEN,
                    root_source(0x60000000u, 0x1000u, kRead | kWrite,
                               RegionKind::DriverMmio),
                    app_gets_mmio, &cap) == Error::Role);
  GrantRequest driver_gets_app{ROOT, DRIVER, ObjectRole::Driver, 0x40800000u,
                               0x1000u, kRead, RegionKind::AppPrivate};
  assert(make_grant(ROOT, ROOT_GEN, DRIVER_GEN, app_private,
                    driver_gets_app, &cap) == Error::Role);
  GrantRequest driver_mmio{ROOT, DRIVER, ObjectRole::Driver, 0x60000000u,
                           0x1000u, kRead | kWrite, RegionKind::DriverMmio};
  auto exact_mmio = root_source(0x60000000u, 0x1000u, kRead | kWrite,
                                RegionKind::DriverMmio);
  assert(make_grant(ROOT, ROOT_GEN, DRIVER_GEN, exact_mmio,
                    driver_mmio, &cap) == Error::Ok);
  driver_mmio.length = 0x800u; // MMIO authority is exact, never subdelegated.
  assert(make_grant(ROOT, ROOT_GEN, DRIVER_GEN, exact_mmio,
                    driver_mmio, &cap) == Error::Range);

  // Source rights, root-only grant authority, no regrant, and stale source checks.
  auto read_only = root_source(0x40801000u, 0x1000u, kRead,
                               RegionKind::ExplicitShared);
  GrantRequest write_request{ROOT, APP, ObjectRole::App, 0x40801000u, 0x1000u,
                             kRead | kWrite, RegionKind::ExplicitShared};
  assert(make_grant(ROOT, ROOT_GEN, APP_GEN, read_only,
                    write_request, &cap) == Error::Rights);
  write_request.rights = kRead | kWrite | kExecute;
  auto rwx_source = root_source(0x40801000u, 0x1000u, kKnownRights,
                                RegionKind::ExplicitShared);
  assert(make_grant(ROOT, ROOT_GEN, APP_GEN, rwx_source,
                    write_request, &cap) == Error::Rights);
  write_request.rights = kRead | kWrite;
  auto child_cap = RegionCapability{0x40801000u, 0x1000u, kRead,
      RegionKind::ExplicitShared, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN};
  write_request.caller_object = DRIVER;
  assert(make_grant(ROOT, ROOT_GEN, APP_GEN, child_cap,
                    write_request, &cap) == Error::Authority);
  write_request.caller_object = ROOT;
  assert(make_grant(ROOT, ROOT_GEN + 1, APP_GEN, read_only,
                    write_request, &cap) == Error::Stale);
  child_cap.issuer_object = DRIVER;
  assert(validate_set(ROOT, ROOT_GEN, DRIVER, DRIVER_GEN, ObjectRole::Driver,
                      &child_cap, 1) == Error::Regrant);

  // Set validation catches stale generations, overlap ambiguity, and PMP overflow.
  RegionCapability shared[2] = {
      {0x40802000u, 0x1000u, kRead, RegionKind::ExplicitShared,
       ROOT, APP, ROOT_GEN, APP_GEN},
      {0x40803000u, 0x1000u, kRead, RegionKind::ExplicitShared,
       ROOT, APP, ROOT_GEN, APP_GEN}};
  assert(validate_set(ROOT, ROOT_GEN, APP, APP_GEN, ObjectRole::App,
                      shared, 2) == Error::Ok);
  assert(validate_set(ROOT, ROOT_GEN, APP, APP_GEN + 1, ObjectRole::App,
                      shared, 2) == Error::Stale);
  shared[1].base = shared[0].base + 0x800u;
  assert(validate_set(ROOT, ROOT_GEN, APP, APP_GEN, ObjectRole::App,
                      shared, 2) == Error::Overlap);

  std::array<RegionCapability, 17> too_many{};
  for (size_t i = 0; i < too_many.size(); ++i) {
    too_many[i] = {static_cast<uint32_t>(0x41000000u + i * 0x2000u),
                   0x1000u, kRead, RegionKind::ExplicitShared,
                   ROOT, APP, ROOT_GEN, APP_GEN};
  }
  assert(validate_set(ROOT, ROOT_GEN, APP, APP_GEN, ObjectRole::App,
                      too_many.data(), too_many.size()) == Error::PmpBudget);
  assert(pmp_entry_cost(0x41000000u, 8u) == 1); // minimum aligned NAPOT
  assert(pmp_entry_cost(0x41000000u, 4u) == 2); // 4-byte TOR window
  assert(!valid_range(0x41000002u, 8u)); // PMP granule is four bytes
  assert(!valid_range(0x41000000u, 2u));
  assert(pmp_entry_cost(0x41000002u, 8u) > kPmpEntries);
  assert(!valid_range(0xFFFFFFF0u, 0x40u));

  // Synthetic linker manifest only: these are test addresses, not candidate-ELF evidence.
  std::array<LayoutWindow, 11> layout = {{
      {0x40000000u, 0x1000u, 0, LayoutKind::KernelTrusted},
      {0x40800000u, 0x1000u, kRead | kExecute, LayoutKind::AppCode},
      {0x40801000u, 0x1000u, kRead | kWrite, LayoutKind::AppData},
      {0x40802000u, 0x1000u, kRead | kWrite, LayoutKind::AppStack},
      {0x40803000u, 0x1000u, kRead | kExecute, LayoutKind::DriverCode},
      {0x40804000u, 0x1000u, kRead | kWrite, LayoutKind::DriverData},
      {0x40805000u, 0x1000u, kRead | kWrite, LayoutKind::DriverStack},
      {0x40806000u, 0x1000u, kRead | kWrite, LayoutKind::ExplicitShared},
      {0x60000000u, 0x1000u, kRead | kWrite, LayoutKind::DriverMmioPolicy},
      {0x40001000u, 0x1000u, 0, LayoutKind::KernelTrusted}, // ledger
      {0x40002000u, 0x1000u, 0, LayoutKind::KernelTrusted}, // trusted/header area
  }};
  std::array<RegionCapability, 9> mapped = {{
      {0x40800000u, 0x1000u, kRead | kExecute, RegionKind::AppPrivate, ROOT, APP, ROOT_GEN, APP_GEN},
      {0x40801000u, 0x1000u, kRead | kWrite, RegionKind::AppPrivate, ROOT, APP, ROOT_GEN, APP_GEN},
      {0x40802000u, 0x1000u, kRead | kWrite, RegionKind::AppPrivate, ROOT, APP, ROOT_GEN, APP_GEN},
      {0x40803000u, 0x1000u, kRead | kExecute, RegionKind::DriverPrivate, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN},
      {0x40804000u, 0x1000u, kRead | kWrite, RegionKind::DriverPrivate, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN},
      {0x40805000u, 0x1000u, kRead | kWrite, RegionKind::DriverPrivate, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN},
      {0x40806000u, 0x1000u, kRead | kWrite, RegionKind::ExplicitShared, ROOT, APP, ROOT_GEN, APP_GEN},
      {0x40806000u, 0x1000u, kRead | kWrite, RegionKind::ExplicitShared, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN},
      {0x60000000u, 0x1000u, kRead | kWrite, RegionKind::DriverMmio, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN},
  }};
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), mapped.data(), mapped.size()) == Error::Ok);

  auto bad_layout = layout;
  for (size_t protected_index : {size_t{0}, size_t{9}, size_t{10}}) {
    bad_layout = layout;
    bad_layout[1].base = bad_layout[protected_index].base; // kernel/ledger/header overlap
    assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                  bad_layout.data(), bad_layout.size(), mapped.data(), mapped.size()) == Error::Overlap);
  }
  bad_layout = layout;
  bad_layout[4].base = bad_layout[1].base; // app and driver private link windows overlap
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                bad_layout.data(), bad_layout.size(), mapped.data(), mapped.size()) == Error::Overlap);

  auto bad_caps = mapped;
  bad_caps[3].base = bad_caps[0].base; // driver tries to claim app-private address
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), bad_caps.data(), bad_caps.size()) == Error::Range);
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), mapped.data(), 7) == Error::Manifest); // shared missing driver grant
  bad_caps = mapped;
  bad_caps[8].base += 4u; bad_caps[8].length -= 4u; // MMIO must equal root policy exactly
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), bad_caps.data(), bad_caps.size()) == Error::Range);
  bad_caps = mapped;
  bad_caps[8].rights = kRead | kExecute; // exceeds MMIO policy rights
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), bad_caps.data(), bad_caps.size()) == Error::Rights);
  bad_caps = mapped;
  bad_caps[2].base += 4u; bad_caps[2].length -= 4u; // owner needs exact self stack grant
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                layout.data(), layout.size(), bad_caps.data(), bad_caps.size()) == Error::Manifest);

  std::array<LayoutWindow, 25> budget_layout{};
  std::array<RegionCapability, 37> budget_caps{};
  for (size_t i = 0; i < layout.size(); ++i) budget_layout[i] = layout[i];
  for (size_t i = 0; i < mapped.size(); ++i) budget_caps[i] = mapped[i];
  size_t window_i = layout.size(), cap_i = mapped.size();
  for (size_t i = 0; i < 14; ++i) {
    const uint32_t base = static_cast<uint32_t>(0x50000000u + i * 16u);
    budget_layout[window_i++] = {base, 8u, kRead | kWrite, LayoutKind::ExplicitShared};
    budget_caps[cap_i++] = {base, 8u, kRead | kWrite, RegionKind::ExplicitShared, ROOT, APP, ROOT_GEN, APP_GEN};
    budget_caps[cap_i++] = {base, 8u, kRead | kWrite, RegionKind::ExplicitShared, ROOT, DRIVER, ROOT_GEN, DRIVER_GEN};
  }
  assert(validate_global_layout(ROOT, ROOT_GEN, APP, APP_GEN, DRIVER, DRIVER_GEN,
                                budget_layout.data(), window_i,
                                budget_caps.data(), cap_i) == Error::PmpBudget);

  uint32_t next = 0;
  assert(revoke(ROOT, ROOT, 41, &next) == Error::Ok && next == 42);
  assert(revoke(DRIVER, ROOT, 41, &next) == Error::Authority);
  assert(revoke(ROOT, ROOT, UINT32_MAX, &next) == Error::GenerationExhausted);

  // PMP window validation: Driver MMIO vs App separation
  pmp_c6_config driver_cfg{
    .code_base = 0x40800000u,
    .code_limit = 0x40801000u,
    .data_base = 0x40810000u,
    .data_limit = 0x40811000u,
    .stack_base = 0x40820000u,
    .stack_limit = 0x40821000u,
    .region_base = 0,
    .region_limit = 0,
    .mmio_base = 0x60000000u,
    .mmio_limit = 0x60001000u,
  };
  assert(pmp_controller::validate(driver_cfg, 0x40830000u, 4096u));

  pmp_c6_config bad_mmio = driver_cfg;
  bad_mmio.mmio_base = 0x40800000u; // overlap with code
  bad_mmio.mmio_limit = 0x40801000u;
  assert(!pmp_controller::validate(bad_mmio, 0x40830000u, 4096u));

  bad_mmio = driver_cfg;
  bad_mmio.mmio_base = 0x40830000u; // overlap with kstack
  bad_mmio.mmio_limit = 0x40831000u;
  assert(!pmp_controller::validate(bad_mmio, 0x40830000u, 4096u));

  bad_mmio = driver_cfg;
  bad_mmio.mmio_base = 0x60000001u; // misaligned
  assert(!pmp_controller::validate(bad_mmio, 0x40830000u, 4096u));

  bad_mmio = driver_cfg;
  bad_mmio.mmio_base = 0x60001000u; // base >= limit
  bad_mmio.mmio_limit = 0x60000000u;
  assert(!pmp_controller::validate(bad_mmio, 0x40830000u, 4096u));

  c6_stage1_object_bounds app_bounds{
    .owner_id = 10,
    .code_base = 0x40800000u,
    .code_limit = 0x40801000u,
    .data_base = 0x40810000u,
    .data_limit = 0x40811000u,
    .stack_base = 0x40820000u,
    .stack_limit = 0x40821000u,
    .u_return_trampoline = 0x40800010u,
    .role = c6_stage1_role::App,
    .mmio_base = 0x60000000u,
    .mmio_limit = 0x60001000u,
  };
  assert(!role_allows_kind(static_cast<ObjectRole>(app_bounds.role), RegionKind::DriverMmio));
  c6_stage1_object_bounds drv_bounds = app_bounds;
  drv_bounds.role = c6_stage1_role::Driver;
  assert(role_allows_kind(static_cast<ObjectRole>(drv_bounds.role), RegionKind::DriverMmio));

  return 0;
}
