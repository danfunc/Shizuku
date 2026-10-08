#ifndef SHIZUKU_STREAM_HPP
#define SHIZUKU_STREAM_HPP
#include <cstdint>

#include "shizuku/freestanding_mem.hpp"
#include "shizuku/object_api.hpp"

namespace shizuku::stream {

constexpr uint32_t LOSSLESS = 1u << 0;
constexpr uintptr_t INVALID_ID = ~(uintptr_t)0;
constexpr uint32_t NO_OWNER = 0xFFFFFFFFu;
constexpr uint32_t CONNECTED = 0xFFFFFFFEu;
constexpr uint32_t KERNEL_OWNER = 0xFFFFFFFDu;
enum struct role : uint32_t { PRODUCER = 0, CONSUMER = 1 };
constexpr uint32_t RESYNC_MARGIN = 2;

// Batch request objects are caller-owned only for the duration of one svc.
// To keep the privileged handler from copying to arbitrary addresses, request
// and record buffers must lie wholly inside the current thread's stack.
struct push_request {
  const void *records;
  uint32_t count;
};
struct pop_request {
  void *records;
  uint32_t count;
  uint32_t lost;
};

struct extent {
  uintptr_t address;
  uint32_t bytes;
  uint32_t sequence;
};

struct dma_window {
  uint32_t count;
  uint32_t room;
  uint32_t source_rd;
};
constexpr dma_window bounded_dma_window(uint32_t source_capacity,
                                        uint32_t destination_capacity,
                                        uint32_t source_count,
                                        uint32_t source_rd,
                                        uint32_t destination_wr,
                                        uint32_t destination_rd) {
  if (source_capacity == 0 || destination_capacity == 0)
    return {0, 0, source_rd};
  if (source_count > source_capacity) {
    source_rd += source_count - source_capacity;
    source_count = source_capacity;
  }
  const uint32_t occupied = destination_wr - destination_rd;
  const uint32_t room = occupied > destination_capacity
                            ? 0
                            : destination_capacity - occupied;
  const uint32_t source_run = source_capacity - source_rd % source_capacity;
  const uint32_t destination_run =
      destination_capacity - destination_wr % destination_capacity;
  uint32_t count = source_count < room ? source_count : room;
  if (count > source_run) count = source_run;
  if (count > destination_run) count = destination_run;
  return {count, room, source_rd};
}

template <typename REC, typename ARCH_T> class basic_handle {
public:
  basic_handle() = default;
  explicit basic_handle(uintptr_t id) : m_id(id) {}
  bool valid() const { return m_id != INVALID_ID; }
  uintptr_t id() const { return m_id; }

  uint32_t push_n(const REC *records, uint32_t count,
                  uintptr_t *error = nullptr) const {
    push_request request{records, count};
    const auto result = ARCH_T::syscall((uintptr_t)object_api::STREAM_PUSH_N,
                                        m_id, (uintptr_t)&request);
    if (error) *error = result.error;
    return result.value > count ? count : (uint32_t)result.value;
  }
  bool push(const REC &record) const { return push_n(&record, 1) == 1; }

  uint32_t pop_n(REC *records, uint32_t count, uint32_t *lost = nullptr,
                 uintptr_t *error = nullptr) const {
    pop_request request{records, count, 0};
    const auto result = ARCH_T::syscall((uintptr_t)object_api::STREAM_POP_N,
                                        m_id, (uintptr_t)&request);
    if (lost) *lost = request.lost;
    if (error) *error = result.error;
    return result.value > count ? count : (uint32_t)result.value;
  }
  bool pop(REC *record, uint32_t *lost = nullptr) const {
    return pop_n(record, 1, lost) == 1;
  }
  uint32_t available() const {
    const auto result = ARCH_T::syscall(
        (uintptr_t)object_api::STREAM_AVAILABLE, m_id);
    return (uint32_t)result.value;
  }

private:
  uintptr_t m_id = INVALID_ID;
};

} // namespace shizuku::stream
#endif // SHIZUKU_STREAM_HPP
