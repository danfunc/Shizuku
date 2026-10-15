#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "tests/composition_link/sample_object.hpp"

// 全体カウントと C ABI シンボルの宣言
namespace shizuku::object_id {
extern const uintptr_t count;
}

extern "C" {
extern const uintptr_t shizuku_object_id_sample_service;
extern const uintptr_t shizuku_object_id_target_service;
extern const uintptr_t shizuku_object_id_count;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "Usage: %s <expected_sample_id> <expected_target_id> <expected_count>\n", argv[0]);
    return 1;
  }

  const uintptr_t expected_sample_id = (uintptr_t)std::strtoul(argv[1], nullptr, 0);
  const uintptr_t expected_target_id = (uintptr_t)std::strtoul(argv[2], nullptr, 0);
  const uintptr_t expected_count = (uintptr_t)std::strtoul(argv[3], nullptr, 0);

  const uintptr_t got_my_id = sample_module::get_my_id();
  const uintptr_t got_target_id = sample_module::get_target_id();
  const uintptr_t got_count = shizuku::object_id::count;

  std::printf("[TEST] sample_module::get_my_id() = %lu (expected %lu)\n",
              (unsigned long)got_my_id, (unsigned long)expected_sample_id);
  std::printf("[TEST] sample_module::get_target_id() = %lu (expected %lu)\n",
              (unsigned long)got_target_id, (unsigned long)expected_target_id);
  std::printf("[TEST] shizuku::object_id::count = %lu (expected %lu)\n",
              (unsigned long)got_count, (unsigned long)expected_count);

  // C++ 名前空間シンボルの解決結果を assert
  assert(got_my_id == expected_sample_id);
  assert(got_target_id == expected_target_id);
  assert(got_count == expected_count);

  // C ABI シンボル (shizuku_object_id_*) の解決結果も照合
  assert(shizuku_object_id_sample_service == expected_sample_id);
  assert(shizuku_object_id_target_service == expected_target_id);
  assert(shizuku_object_id_count == expected_count);

  std::printf("[TEST] All assertions passed successfully.\n");
  return 0;
}
