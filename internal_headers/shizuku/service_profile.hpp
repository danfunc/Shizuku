#ifndef SHIZUKU_SERVICE_PROFILE_HPP
#define SHIZUKU_SERVICE_PROFILE_HPP
#include <cstdint>
namespace shizuku::service_profile {
struct maxima {
  uint32_t handler_max = 0, handler_samples = 0;
  uint32_t mask_max = 0, mask_samples = 0;
  void handler(uint32_t started, uint32_t ended) {
    const uint32_t elapsed = ended - started;
    if (elapsed > handler_max) handler_max = elapsed;
    ++handler_samples;
  }
  void mask(uint32_t started, uint32_t ended) {
    const uint32_t elapsed = ended - started;
    if (elapsed > mask_max) mask_max = elapsed;
    ++mask_samples;
  }
};
void reset(); // current core, in Thread mode; doesn't reset another core's open span
maxima read(); // current core, coherent snapshot
bool available();
bool cycles();
}
extern "C" void shizuku_service_handler_begin();
extern "C" void shizuku_service_handler_end();
extern "C" uint32_t shizuku_service_profile_now();
extern "C" void shizuku_service_mask_span(uint32_t started);
#endif
