#include "shizuku/memory_managers/freestanding.hpp"
#include <cstddef>

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

namespace shizuku {
namespace memory_managers {

// ESP32-C5/C6 pre-init static backing arena. The BLE C5 controller-init profile
// has a separate, bounded object arena and must leave DRAM for the BT controller.
// start_cpu0 置換により IDF libc/heap 初期化をバイパスしているため、
// 実行時に未初期化の libc malloc() を呼ばず、静的メモリ領域から安全に割り当てる。
#if defined(SHIZUKU_C5_BLE)
constexpr size_t kFreestandingHeapBytes = 64 * 1024;
#else
constexpr size_t kFreestandingHeapBytes = 128 * 1024;
#endif
alignas(8) static uint8_t s_c6_kernel_heap[kFreestandingHeapBytes];
static uintptr_t s_allocated_offset = 0;

templates::result<void *> freestanding::kernel_malloc(uintptr_t size) {
  // size == 0 または単調 arena 残容量超過を先にチェックし、size + 7 の加算オーバーフローを防止
  if (size == 0 || size > sizeof(s_c6_kernel_heap) - s_allocated_offset) {
    return {result_code::allocation_failed, "c6 static heap exhausted or invalid size\n"};
  }
  const uintptr_t aligned_size = (size + 7u) & ~static_cast<uintptr_t>(7u);
  if (aligned_size > sizeof(s_c6_kernel_heap) - s_allocated_offset) {
    return {result_code::allocation_failed, "c6 static heap exhausted\n"};
  }
  void *addr = &s_c6_kernel_heap[s_allocated_offset];
  s_allocated_offset += aligned_size;
  return addr;
}

templates::result<void> freestanding::kernel_free(void *ptr) {
  if (ptr == nullptr) {
    return {};
  }
  // 単調 bump arena では個別解放をサポートしない。
  // 無効な解放要求を欺瞞的に成功 (no-op) とせず、明確にエラーを返す。
  return {result_code::allocation_failed, "individual free not supported on c6 monotonic arena\n"};
}

templates::result<void> freestanding::init() {
  // 制約: システム初期化 (boot) 時にのみ呼ぶこと。
  // 稼働中の呼び出しは既存オブジェクトのポインタを破壊・再利用するため禁止。
  s_allocated_offset = 0;
  return {};
}

} // namespace memory_managers
} // namespace shizuku
