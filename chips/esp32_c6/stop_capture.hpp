#ifndef SHIZUKU_CHIPS_ESP32_C6_STOP_CAPTURE_HPP
#define SHIZUKU_CHIPS_ESP32_C6_STOP_CAPTURE_HPP

#include <cstddef>
#include <cstdint>

namespace shizuku::chips::esp32_c6::stop_capture {

constexpr uint32_t kMagic = 0x50434653u;  // "SFCP" in little-endian RAM.
constexpr uint32_t kVersion = 1u;
constexpr size_t kFrameWords = 36u;
constexpr size_t kFrameBytes = kFrameWords * sizeof(uint32_t);

enum valid_field : uint32_t {
  kFrameValid = 1u << 0,
  kContextValid = 1u << 1,
};

struct ram_range {
  uintptr_t begin;
  uintptr_t end;  // Half-open: [begin, end).
};

// Type-erased RV32 context layout. Offsets come from the caller's actual C6
// context type (for example offsetof(context_t, sp/privileged)); this header
// deliberately does not include kernel or SDK headers.
struct context_layout {
  size_t object_bytes;
  size_t sp_offset;
  size_t privileged_offset;
};

template <class Context>
constexpr context_layout layout_of() {
  if constexpr (requires(const Context &c) {
                  c.sp;
                  c.privileged;
                }) {
    return context_layout{sizeof(Context), offsetof(Context, sp),
                          offsetof(Context, privileged)};
  } else {
    static_assert(sizeof(Context) == 0,
                  "Context type must have 'sp' and 'privileged' members");
  }
}

struct capture_input {
  uint32_t status;
  uint32_t phase;
  uint32_t caller_pc;
  uintptr_t frame_address;
  uintptr_t context_address;
  uint32_t mcause;
  uint32_t mepc;
  uint32_t mtval;
  uint32_t mstatus;
  uint32_t sp;
  uint32_t ra;
  uint32_t trap_depth;
  uint32_t irq_depth;
  uint32_t boot_marker;
  uint32_t boot_generation;
};

struct capture_record {
  uint32_t magic;
  uint32_t version;
  uint32_t valid_mask;
  uint32_t status;
  uint32_t phase;
  uint32_t caller_pc;
  uintptr_t frame_address;
  uint32_t frame_words[kFrameWords];
  uintptr_t context_address;
  uint32_t context_sp;
  uint32_t context_privileged;
  uint32_t mcause;
  uint32_t mepc;
  uint32_t mtval;
  uint32_t mstatus;
  uint32_t sp;
  uint32_t ra;
  uint32_t trap_depth;
  uint32_t irq_depth;
  uint32_t boot_marker;
  uint32_t boot_generation;
};

static_assert(kFrameBytes == 144u, "C6 trap frame must be 144 bytes");
constexpr size_t kCaptureRecordBytes = 54u * sizeof(uint32_t) +
                                       2u * sizeof(uintptr_t);
constexpr size_t kCaptureRecordAlignment = alignof(uintptr_t);
static_assert(sizeof(capture_record) ==
                  ((kCaptureRecordBytes + kCaptureRecordAlignment - 1u) /
                   kCaptureRecordAlignment) *
                      kCaptureRecordAlignment,
              "stop capture record layout changed");
static_assert(offsetof(capture_record, frame_words) ==
                  6u * sizeof(uint32_t) + sizeof(uintptr_t),
              "frame words must follow the fixed capture header");

inline bool is_valid(const volatile capture_record &record) {
  return record.magic == kMagic && record.version == kVersion;
}

inline const volatile capture_record *validated_record(const volatile capture_record &record) {
  return is_valid(record) ? &record : nullptr;
}

// Raw .noinit storage wrapper that enforces magic/version validation before payload access.
template <typename T, uint32_t ExpectedMagic = kMagic, uint32_t ExpectedVersion = kVersion>
class retained_slot {
 public:
  volatile T storage;

  constexpr bool is_valid() const volatile {
    if constexpr (requires(const volatile T &v) { v.magic; v.version; }) {
      return storage.magic == ExpectedMagic && storage.version == ExpectedVersion;
    } else if constexpr (requires(const volatile T &v) { v.magic; }) {
      return storage.magic == ExpectedMagic;
    } else {
      return true;
    }
  }

  const volatile T *get_if_valid() const volatile {
    return is_valid() ? &storage : nullptr;
  }

  volatile T *get_if_valid() volatile {
    return is_valid() ? &storage : nullptr;
  }

  void invalidate() volatile {
    if constexpr (requires(volatile T &v) { v.magic; }) {
      storage.magic = 0u;
    }
  }
};

inline bool range_contains(const ram_range &range, uintptr_t address,
                           size_t bytes, size_t alignment = 1u) {
  if (range.begin >= range.end || bytes == 0u || alignment == 0u ||
      address % alignment != 0u || address < range.begin ||
      address > UINTPTR_MAX - bytes)
    return false;
  const uintptr_t end = address + bytes;
  return end <= range.end;
}

inline uint32_t capture(volatile capture_record &out, const capture_input &in,
                        const ram_range &frame_ram,
                        const ram_range &context_ram,
                        const context_layout &layout) {
  // Invalidate any prior snapshot first. A reader accepts it only after magic
  // is written last, once all fields and optional copies are complete.
  out.magic = 0u;
  out.version = kVersion;
  out.valid_mask = 0u;
  out.status = in.status;
  out.phase = in.phase;
  out.caller_pc = in.caller_pc;
  out.frame_address = in.frame_address;
  out.context_address = in.context_address;
  out.context_sp = 0u;
  out.context_privileged = 0u;
  out.mcause = in.mcause;
  out.mepc = in.mepc;
  out.mtval = in.mtval;
  out.mstatus = in.mstatus;
  out.sp = in.sp;
  out.ra = in.ra;
  out.trap_depth = in.trap_depth;
  out.irq_depth = in.irq_depth;
  out.boot_marker = in.boot_marker;
  out.boot_generation = in.boot_generation;
  for (size_t i = 0; i < kFrameWords; ++i) out.frame_words[i] = 0u;

  if (range_contains(frame_ram, in.frame_address, kFrameBytes,
                     alignof(uint32_t))) {
    const volatile uint32_t *source =
        reinterpret_cast<const volatile uint32_t *>(in.frame_address);
    for (size_t i = 0; i < kFrameWords; ++i)
      out.frame_words[i] = source[i];
    out.valid_mask |= kFrameValid;
  }

  const bool layout_valid =
      layout.object_bytes != 0u && layout.sp_offset % alignof(uint32_t) == 0u &&
      layout.sp_offset <= layout.object_bytes &&
      sizeof(uint32_t) <= layout.object_bytes - layout.sp_offset &&
      layout.privileged_offset < layout.object_bytes;
  if (layout_valid &&
      range_contains(context_ram, in.context_address, layout.object_bytes,
                     alignof(uint32_t))) {
    const volatile uint8_t *source =
        reinterpret_cast<const volatile uint8_t *>(in.context_address);
    const volatile uint32_t *saved_sp = reinterpret_cast<const volatile uint32_t *>(
        in.context_address + layout.sp_offset);
    out.context_sp = *saved_sp;
    out.context_privileged = source[layout.privileged_offset];
    out.valid_mask |= kContextValid;
  }

  out.magic = kMagic;
  return out.valid_mask;
}

template <class Context>
inline uint32_t capture(volatile capture_record &out, const capture_input &in,
                        const ram_range &frame_ram,
                        const ram_range &context_ram) {
  return capture(out, in, frame_ram, context_ram, layout_of<Context>());
}

}  // namespace shizuku::chips::esp32_c6::stop_capture

#endif  // SHIZUKU_CHIPS_ESP32_C6_STOP_CAPTURE_HPP
