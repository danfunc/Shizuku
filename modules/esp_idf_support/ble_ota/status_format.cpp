#include "status_format.hpp"
#include <cstdio>

namespace shizuku::esp_idf_support::ble_ota {
status_delivery format_status(const xno::ota::protocol::Event &event,
                              char *out, size_t capacity, size_t *length) {
  if (!out || capacity == 0 || !length) return status_delivery::none;
  int n = -1;
  using xno::ota::protocol::EventType;
  switch (event.type) {
  case EventType::Ready:
    n = std::snprintf(out, capacity, "ready\n");
    break;
  case EventType::MissingCount: {
    const uint32_t chunks = (event.total_bytes + 4095u) / 4096u;
    n = std::snprintf(out, capacity, "NEED n=%lu of=%lu ok=%lu bad=0 r=1\n",
        static_cast<unsigned long>(event.missing_count),
        static_cast<unsigned long>(chunks),
        static_cast<unsigned long>(chunks - event.missing_count));
    break;
  }
  case EventType::MissingSequence:
    n = std::snprintf(out, capacity, "NEEDSEQ %lu\n",
                      static_cast<unsigned long>(event.sequence));
    break;
  case EventType::MissingEnd:
    n = std::snprintf(out, capacity, "NEEDEND\n");
    break;
  case EventType::Done:
    n = std::snprintf(out, capacity, "done: %lu bytes crc=%08lx OK\n",
        static_cast<unsigned long>(event.total_bytes),
        static_cast<unsigned long>(event.image_crc32));
    break;
  case EventType::CommitReady:
    n = std::snprintf(out, capacity, "commit: OK\n");
    break;
  case EventType::Reset:
    n = std::snprintf(out, capacity, "reset: OK\n");
    break;
  case EventType::Failed:
    n = std::snprintf(out, capacity, "FAILED: OTA rejected\n");
    break;
  }
  if (n < 0 || static_cast<size_t>(n) >= capacity) return status_delivery::none;
  *length = static_cast<size_t>(n);
  return event.type == EventType::CommitReady ? status_delivery::indication
                                               : status_delivery::notification;
}
}
