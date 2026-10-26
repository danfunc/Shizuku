#ifndef SHIZUKU_INTERNAL_HEADERS_SHIZUKU_REGION_CAPABILITY_HPP
#define SHIZUKU_INTERNAL_HEADERS_SHIZUKU_REGION_CAPABILITY_HPP

#include <cstddef>
#include <cstdint>

namespace shizuku::region_capability {

inline constexpr uint32_t kRead = 1u << 0;
inline constexpr uint32_t kWrite = 1u << 1;
inline constexpr uint32_t kExecute = 1u << 2;
inline constexpr uint32_t kKnownRights = kRead | kWrite | kExecute;
inline constexpr size_t kPmpEntries = 16;

enum class ObjectRole : uint8_t { Root, App, Driver };
enum class RegionKind : uint8_t { AppPrivate, DriverPrivate, ExplicitShared, DriverMmio };
enum class Error : uint8_t {
  Ok,
  Authority,
  Stale,
  Range,
  Rights,
  Role,
  Kind,
  Regrant,
  Overlap,
  PmpBudget,
  GenerationExhausted,
  Manifest,
};

// This is a host-validated proposal type only. It is not yet stored in object_t
// or consumed by the dispatch/trap path. Runtime callers must obtain `source`
// from a protected root-owned policy table; never accept it from syscall input.
struct RegionCapability {
  uint32_t base;
  uint32_t length;
  uint32_t rights;
  RegionKind kind;
  uint32_t issuer_object;
  uint32_t target_object;
  uint32_t issuer_generation;
  uint32_t target_generation;
};

struct GrantRequest {
  uint32_t caller_object;
  uint32_t target_object;
  ObjectRole target_role;
  uint32_t base;
  uint32_t length;
  uint32_t rights;
  RegionKind kind;
};

constexpr bool add_overflow(uint32_t base, uint32_t length) noexcept {
  return static_cast<uint64_t>(base) + length > (uint64_t{1} << 32);
}
constexpr bool valid_range(uint32_t base, uint32_t length) noexcept {
  return base != 0 && length != 0 && (base & 3u) == 0 &&
         (length & 3u) == 0 && !add_overflow(base, length);
}
constexpr bool contains(uint32_t outer_base, uint32_t outer_length,
                        uint32_t inner_base, uint32_t inner_length) noexcept {
  if (!valid_range(outer_base, outer_length) ||
      !valid_range(inner_base, inner_length)) return false;
  const uint64_t outer_end = static_cast<uint64_t>(outer_base) + outer_length;
  const uint64_t inner_end = static_cast<uint64_t>(inner_base) + inner_length;
  return outer_base <= inner_base && inner_end <= outer_end;
}
constexpr bool overlaps(uint32_t a_base, uint32_t a_length,
                        uint32_t b_base, uint32_t b_length) noexcept {
  if (!valid_range(a_base, a_length) || !valid_range(b_base, b_length)) return true;
  const uint64_t a_end = static_cast<uint64_t>(a_base) + a_length;
  const uint64_t b_end = static_cast<uint64_t>(b_base) + b_length;
  return static_cast<uint64_t>(a_base) < b_end &&
         static_cast<uint64_t>(b_base) < a_end;
}
constexpr bool rights_subset(uint32_t requested, uint32_t ceiling) noexcept {
  const bool wx = (requested & (kWrite | kExecute)) == (kWrite | kExecute);
  return requested != 0 && !wx && (requested & ~kKnownRights) == 0 &&
         (requested & ~ceiling) == 0;
}
constexpr bool role_allows_kind(ObjectRole role, RegionKind kind) noexcept {
  switch (kind) {
    case RegionKind::AppPrivate: return role == ObjectRole::App;
    case RegionKind::DriverPrivate:
    case RegionKind::DriverMmio: return role == ObjectRole::Driver;
    case RegionKind::ExplicitShared:
      return role == ObjectRole::App || role == ObjectRole::Driver;
  }
  return false;
}
constexpr size_t pmp_entry_cost(uint32_t base, uint32_t length) noexcept {
  if (!valid_range(base, length)) return kPmpEntries + 1;
  const bool power_of_two = (length & (length - 1u)) == 0;
  // C6 NAPOT needs at least 8 bytes; smaller 4-byte-granule windows use TOR.
  return length >= 8u && power_of_two && (base & (length - 1u)) == 0 ? 1u : 2u;
}

// A root source is the only grant authority. Child-issued capabilities can be
// consumed but never become sources for another grant (no re-delegation).
constexpr Error make_grant(uint32_t root_object, uint32_t root_generation,
                           uint32_t current_target_generation,
                           const RegionCapability& source,
                           const GrantRequest& request,
                           RegionCapability* out) noexcept {
  if (out == nullptr || request.caller_object != root_object ||
      source.issuer_object != root_object || source.target_object != root_object)
    return Error::Authority;
  if (source.issuer_generation != root_generation ||
      source.target_generation != root_generation)
    return Error::Stale;
  if (!role_allows_kind(request.target_role, request.kind)) return Error::Role;
  if (request.kind != source.kind) return Error::Kind;
  if (!valid_range(source.base, source.length) ||
      !contains(source.base, source.length, request.base, request.length))
    return Error::Range;
  if (!rights_subset(source.rights, kKnownRights) ||
      !rights_subset(request.rights, source.rights) ||
      (request.kind == RegionKind::DriverMmio &&
       (request.rights & kExecute) != 0)) return Error::Rights;
  // MMIO grants must match a root-owned exact window; no subrange broadening.
  if (request.kind == RegionKind::DriverMmio &&
      (request.base != source.base || request.length != source.length))
    return Error::Range;
  *out = {request.base, request.length, request.rights, request.kind,
          root_object, request.target_object, root_generation,
          current_target_generation};
  return Error::Ok;
}

// This rejects overlaps within one object's effective allow-list. A separate
// global linker/manifest check must prove it does not overlap kernel/trusted
// regions or another object's private regions before runtime wiring.
constexpr Error validate_set(uint32_t root_object, uint32_t root_generation,
                             uint32_t target_object, uint32_t target_generation,
                             ObjectRole target_role,
                             const RegionCapability* caps, size_t count) noexcept {
  if (count != 0 && caps == nullptr) return Error::Range;
  size_t entries = 0;
  for (size_t i = 0; i < count; ++i) {
    const RegionCapability& cap = caps[i];
    if (cap.issuer_object != root_object || cap.target_object != target_object)
      return Error::Regrant;
    if (cap.issuer_generation != root_generation ||
        cap.target_generation != target_generation) return Error::Stale;
    if (!role_allows_kind(target_role, cap.kind)) return Error::Role;
    if (!valid_range(cap.base, cap.length)) return Error::Range;
    if (!rights_subset(cap.rights, kKnownRights)) return Error::Rights;
    for (size_t j = 0; j < i; ++j) {
      if (overlaps(cap.base, cap.length, caps[j].base, caps[j].length))
        return Error::Overlap;
    }
    const size_t cost = pmp_entry_cost(cap.base, cap.length);
    if (cost > kPmpEntries - entries) return Error::PmpBudget;
    entries += cost;
  }
  return Error::Ok;
}

constexpr Error revoke(uint32_t caller_object, uint32_t root_object,
                       uint32_t current_generation,
                       uint32_t* next_generation) noexcept {
  if (caller_object != root_object || next_generation == nullptr)
    return Error::Authority;
  if (current_generation == UINT32_MAX) return Error::GenerationExhausted;
  *next_generation = current_generation + 1u;
  return Error::Ok;
}

constexpr bool is_live(const RegionCapability& cap, uint32_t root_object,
                       uint32_t root_generation, uint32_t target_object,
                       uint32_t target_generation) noexcept {
  return cap.issuer_object == root_object && cap.target_object == target_object &&
         cap.issuer_generation == root_generation &&
         cap.target_generation == target_generation;
}

enum class LayoutKind : uint8_t {
  KernelTrusted,
  AppCode,
  AppData,
  AppStack,
  DriverCode,
  DriverData,
  DriverStack,
  ExplicitShared,
  DriverMmioPolicy,
};
struct LayoutWindow {
  uint32_t base;
  uint32_t length;
  uint32_t max_rights;
  LayoutKind kind;
};

constexpr bool layout_kind_matches(RegionKind cap_kind, ObjectRole role,
                                   LayoutKind window_kind) noexcept {
  switch (cap_kind) {
    case RegionKind::AppPrivate:
      return role == ObjectRole::App &&
             (window_kind == LayoutKind::AppCode ||
              window_kind == LayoutKind::AppData ||
              window_kind == LayoutKind::AppStack);
    case RegionKind::DriverPrivate:
      return role == ObjectRole::Driver &&
             (window_kind == LayoutKind::DriverCode ||
              window_kind == LayoutKind::DriverData ||
              window_kind == LayoutKind::DriverStack);
    case RegionKind::ExplicitShared:
      return window_kind == LayoutKind::ExplicitShared &&
             (role == ObjectRole::App || role == ObjectRole::Driver);
    case RegionKind::DriverMmio:
      return role == ObjectRole::Driver &&
             window_kind == LayoutKind::DriverMmioPolicy;
  }
  return false;
}
constexpr bool segment_rights_valid(const LayoutWindow& w) noexcept {
  switch (w.kind) {
    case LayoutKind::KernelTrusted: return w.max_rights == 0;
    case LayoutKind::AppCode:
    case LayoutKind::DriverCode:
      return w.max_rights == (kRead | kExecute);
    case LayoutKind::AppData:
    case LayoutKind::AppStack:
    case LayoutKind::DriverData:
    case LayoutKind::DriverStack:
    case LayoutKind::DriverMmioPolicy:
      return w.max_rights == (kRead | kWrite);
    case LayoutKind::ExplicitShared:
      return rights_subset(w.max_rights, kKnownRights);
  }
  return false;
}
constexpr bool has_exact_shared_grant(const RegionCapability* caps, size_t cap_count,
                                      uint32_t object, uint32_t base,
                                      uint32_t length) noexcept {
  for (size_t i = 0; i < cap_count; ++i) {
    const auto& c = caps[i];
    if (c.target_object == object && c.kind == RegionKind::ExplicitShared &&
        c.base == base && c.length == length) return true;
  }
  return false;
}
constexpr bool has_exact_private_grant(const RegionCapability* caps, size_t cap_count,
                                       uint32_t object, RegionKind kind,
                                       const LayoutWindow& window) noexcept {
  for (size_t i = 0; i < cap_count; ++i) {
    const auto& c = caps[i];
    if (c.target_object == object && c.kind == kind &&
        c.base == window.base && c.length == window.length &&
        c.rights == window.max_rights) return true;
  }
  return false;
}

// Candidate-ELF-fed global gate. Synthetic host tests exercise the rules; this
// cannot establish real isolation until every window comes from a verified map.
constexpr Error validate_global_layout(uint32_t root_object,
                                       uint32_t root_generation,
                                       uint32_t app_object, uint32_t app_generation,
                                       uint32_t driver_object, uint32_t driver_generation,
                                       const LayoutWindow* windows, size_t window_count,
                                       const RegionCapability* caps, size_t cap_count) noexcept {
  if ((window_count != 0 && windows == nullptr) ||
      (cap_count != 0 && caps == nullptr)) return Error::Manifest;
  bool seen[7] = {};
  for (size_t i = 0; i < window_count; ++i) {
    const auto& w = windows[i];
    if (!valid_range(w.base, w.length) || !segment_rights_valid(w)) return Error::Manifest;
    if (w.kind != LayoutKind::KernelTrusted) {
      const size_t slot = static_cast<size_t>(w.kind) - 1u;
      if (slot < 7) seen[slot] = true;
    }
    for (size_t j = 0; j < i; ++j) {
      if (overlaps(w.base, w.length, windows[j].base, windows[j].length))
        return Error::Overlap;
    }
  }
  // Each object has separate code, data, and stack linker windows.
  for (size_t i = 1; i <= 6; ++i) if (!seen[i - 1]) return Error::Manifest;

  size_t app_entries = 0, driver_entries = 0;
  for (size_t i = 0; i < cap_count; ++i) {
    const auto& c = caps[i];
    if (c.issuer_object != root_object) return Error::Regrant;
    if (c.issuer_generation != root_generation) return Error::Stale;
    ObjectRole role;
    uint32_t expected_generation;
    if (c.target_object == app_object) {
      role = ObjectRole::App; expected_generation = app_generation;
    } else if (c.target_object == driver_object) {
      role = ObjectRole::Driver; expected_generation = driver_generation;
    } else {
      return Error::Role;
    }
    if (c.target_generation != expected_generation) return Error::Stale;
    if (!role_allows_kind(role, c.kind)) return Error::Role;
    if (!valid_range(c.base, c.length) || !rights_subset(c.rights, kKnownRights))
      return Error::Rights;
    bool matched = false;
    for (size_t j = 0; j < window_count; ++j) {
      const auto& w = windows[j];
      if (!layout_kind_matches(c.kind, role, w.kind) ||
          !contains(w.base, w.length, c.base, c.length)) continue;
      if (!rights_subset(c.rights, w.max_rights)) return Error::Rights;
      if (c.kind == RegionKind::DriverMmio &&
          (c.base != w.base || c.length != w.length)) return Error::Range;
      matched = true;
      break;
    }
    if (!matched) return Error::Range;
    size_t& used = role == ObjectRole::App ? app_entries : driver_entries;
    const size_t cost = pmp_entry_cost(c.base, c.length);
    if (cost > kPmpEntries - used) return Error::PmpBudget;
    used += cost;
    for (size_t j = 0; j < i; ++j) {
      if (caps[j].target_object == c.target_object &&
          overlaps(c.base, c.length, caps[j].base, caps[j].length))
        return Error::Overlap;
    }
  }
  // Private code/data/stack must each have an exact owner grant.
  for (size_t i = 0; i < window_count; ++i) {
    const auto& w = windows[i];
    if (w.kind == LayoutKind::AppCode || w.kind == LayoutKind::AppData ||
        w.kind == LayoutKind::AppStack) {
      if (!has_exact_private_grant(caps, cap_count, app_object,
                                   RegionKind::AppPrivate, w)) return Error::Manifest;
    } else if (w.kind == LayoutKind::DriverCode || w.kind == LayoutKind::DriverData ||
               w.kind == LayoutKind::DriverStack) {
      if (!has_exact_private_grant(caps, cap_count, driver_object,
                                   RegionKind::DriverPrivate, w)) return Error::Manifest;
    }
  }
  // Shared memory is shared only when both objects have an exact grant.
  for (size_t i = 0; i < window_count; ++i) {
    const auto& w = windows[i];
    if (w.kind != LayoutKind::ExplicitShared) continue;
    if (!has_exact_shared_grant(caps, cap_count, app_object, w.base, w.length) ||
        !has_exact_shared_grant(caps, cap_count, driver_object, w.base, w.length))
      return Error::Manifest;
  }
  return Error::Ok;
}

}  // namespace shizuku::region_capability

#endif
