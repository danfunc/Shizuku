#include "ota_lifecycle.hpp"
#include <cassert>

int main() {
  using namespace shizuku::esp_idf_support::ble_ota;
  const phase begin_error = after_begin(false);
  assert(begin_error == phase::idle);
  assert(!may_activate(begin_error));

  const phase writing = after_begin(true);
  assert(writing == phase::writing);
  const phase bad_image = after_validate(writing, false);
  assert(bad_image == phase::failed);
  assert(!may_activate(bad_image));
  assert(after_abort() == phase::idle);

  const phase good_image = after_validate(writing, true);
  assert(good_image == phase::validated);
  assert(may_activate(good_image));
  assert(!may_activate(phase::activated));
  return 0;
}
