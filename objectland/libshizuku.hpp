#ifndef SHIZUKU_OBJECTLAND_LIBSHIZUKU_HPP
#define SHIZUKU_OBJECTLAND_LIBSHIZUKU_HPP

#include "shizuku_abi.h"

namespace shizuku::objectland {

// ===========================================================================
//  Freestanding Memory Helpers
// ===========================================================================
//  ★非特権のコードは libc / pico-sdk の memcpy・memset を呼んではいけない。
//    RP2040 では bootrom (0x0000xxxx, 特権専用) へ飛び、MPU に落とされるため。
//    ループ内の空 asm は GCC による memcpy/memset 呼出しへの書き戻しを防ぐ。
#define SHIZUKU_OBJ_INLINE [[gnu::always_inline]] inline

SHIZUKU_OBJ_INLINE void freestanding_memcpy(void *dst, const void *src, size_t bytes) {
  uint8_t *d = static_cast<uint8_t *>(dst);
  const uint8_t *s = static_cast<const uint8_t *>(src);
  if ((((uintptr_t)d | (uintptr_t)s | bytes) & 3u) == 0) {
    uint32_t *dw = reinterpret_cast<uint32_t *>(d);
    const uint32_t *sw = reinterpret_cast<const uint32_t *>(s);
    for (size_t i = 0; i < bytes / 4; ++i) {
      dw[i] = sw[i];
      __asm__ volatile("" ::: "memory");
    }
    return;
  }
  for (size_t i = 0; i < bytes; ++i) {
    d[i] = s[i];
    __asm__ volatile("" ::: "memory");
  }
}

SHIZUKU_OBJ_INLINE void freestanding_memset(void *dst, uint8_t value, size_t bytes) {
  uint8_t *d = static_cast<uint8_t *>(dst);
  for (size_t i = 0; i < bytes; ++i) {
    d[i] = value;
    __asm__ volatile("" ::: "memory");
  }
}

template <typename T>
SHIZUKU_OBJ_INLINE void copy_record(T *dst, const T *src) {
  freestanding_memcpy(dst, src, sizeof(T));
}

// ===========================================================================
//  Freestanding Integer Arithmetic
// ===========================================================================
//  ARMv6-M (Cortex-M0+) にはハードウェア除算命令がないため、% 演算子を使うと
//  通常は libgcc の __aeabi_uidivmod が呼ばれる。-nostdlib 環境でも動作するよう
//  ソフトウェア除算ルーチンを提供する。
SHIZUKU_OBJ_INLINE uint32_t freestanding_mod(uint32_t n, uint32_t d) {
  if (d == 0) return 0;
  if ((d & (d - 1)) == 0) {
    return n & (d - 1);
  }
  uint32_t r = 0;
  for (int i = 31; i >= 0; --i) {
    r <<= 1;
    r |= (n >> i) & 1;
    if (r >= d) {
      r -= d;
    }
  }
  return r;
}

// ===========================================================================
//  Minimal Stream Operations
// ===========================================================================
template <typename REC>
SHIZUKU_OBJ_INLINE bool stream_push(shizuku_stream_descriptor *desc, const REC &record) {
  const uint32_t capacity = desc->capacity;
  const uint32_t wr = desc->wr;
  const uint32_t rd = __atomic_load_n(&desc->rd, __ATOMIC_ACQUIRE);
  if ((desc->flags & SHIZUKU_STREAM_LOSSLESS) && (wr - rd) >= capacity) {
    return false;
  }
  REC *slots = static_cast<REC *>(desc->base);
  const uint32_t slot_index = freestanding_mod(wr, capacity);
  copy_record(&slots[slot_index], &record);
  __atomic_store_n(&desc->wr, wr + 1, __ATOMIC_RELEASE);
  return true;
}

// ===========================================================================
//  SVC Syscall Helper
// ===========================================================================
SHIZUKU_OBJ_INLINE shizuku_syscall_result syscall(uintptr_t number,
                                                 uintptr_t a1 = 0,
                                                 uintptr_t a2 = 0,
                                                 uintptr_t a3 = 0,
                                                 uintptr_t a4 = 0) {
  return shizuku_svc(number, a1, a2, a3, a4);
}

} // namespace shizuku::objectland

#endif // SHIZUKU_OBJECTLAND_LIBSHIZUKU_HPP
