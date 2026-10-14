#include "status_format.hpp"
#include <cassert>
#include <cstring>

int main() {
  using namespace xno::ota::protocol;
  using shizuku::esp_idf_support::ble_ota::format_status;
  using shizuku::esp_idf_support::ble_ota::status_delivery;
  char out[96]{};
  size_t length = 0;
  assert(format_status({EventType::MissingCount, 600608, 0, 2, 0, 0},
                      out, sizeof(out), &length) == status_delivery::notification);
  assert(std::strcmp(out, "NEED n=2 of=147 ok=145 bad=0 r=1\n") == 0);
  assert(format_status({EventType::MissingSequence, 600608, 0, 2, 146, 0},
                      out, sizeof(out), &length) == status_delivery::notification);
  assert(std::strcmp(out, "NEEDSEQ 146\n") == 0);
  assert(format_status({EventType::Done, 600608, 0x1234ABCD, 0, 0, 0},
                      out, sizeof(out), &length) == status_delivery::notification);
  assert(std::strcmp(out, "done: 600608 bytes crc=1234abcd OK\n") == 0);
  assert(format_status({EventType::CommitReady, 600608, 0x1234ABCD, 0, 0, 0},
                      out, sizeof(out), &length) == status_delivery::indication);
  assert(std::strcmp(out, "commit: OK\n") == 0);
  return 0;
}
