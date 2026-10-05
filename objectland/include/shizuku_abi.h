#ifndef SHIZUKU_OBJECTLAND_ABI_H
#define SHIZUKU_OBJECTLAND_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
//  Shizuku Objectland Language-Neutral ABI
// ===========================================================================
//  将来 Rust からも全く同一の ABI を利用するため、公開 ABI は言語非依存の
//  SVC 番号、レジスタ規約、descriptor layout として定義する。

// SVC 番号 (object_api.hpp に完全一致)
enum shizuku_svc_number {
  SHIZUKU_SVC_CREATE_OBJECT = 1,
  SHIZUKU_SVC_EXIT_METHOD = 2,
  SHIZUKU_SVC_EXPORT_METHOD = 3,
  SHIZUKU_SVC_CALL_METHOD = 4,
  SHIZUKU_SVC_GET_CURRENT_OBJECT = 5,
  SHIZUKU_SVC_GET_CALLER_OBJECT = 6,
  SHIZUKU_SVC_SPAWN = 7,
  SHIZUKU_SVC_YIELD = 8,
  SHIZUKU_SVC_RUN_FOR = 9,
  SHIZUKU_SVC_EXIT_THREAD = 10,
  SHIZUKU_SVC_SLEEP_US = 11,
  SHIZUKU_SVC_SET_BUDGET = 12,
  SHIZUKU_SVC_MEMORY_ALLOCATE = 13,
  SHIZUKU_SVC_MEMORY_RELEASE = 14,
  SHIZUKU_SVC_MEMORY_HAND_OVER = 15,
  SHIZUKU_SVC_MEMORY_OWNER = 16,
  SHIZUKU_SVC_DECLARE_NAME = 17,
  SHIZUKU_SVC_OBJECT_NAME = 18,
  SHIZUKU_SVC_STREAM_CREATE = 19,
  SHIZUKU_SVC_STREAM_OPEN = 20,
  SHIZUKU_SVC_STREAM_BIND = 21,
  SHIZUKU_SVC_STREAM_CONNECT = 22,
  SHIZUKU_SVC_GRANT_REGION = 23,
  SHIZUKU_SVC_SET_OBJECT_AFFINITY = 24,
  SHIZUKU_SVC_KILL_THREAD = 25,
  SHIZUKU_SVC_STREAM_DISCONNECT = 26,
  SHIZUKU_SVC_FORWARD_CHILD_EXIT = 27,
};

// Stream 定数
#define SHIZUKU_STREAM_LOSSLESS (1u << 0)
#define SHIZUKU_STREAM_NO_OWNER 0xFFFFFFFFu
#define SHIZUKU_STREAM_CONNECTED 0xFFFFFFFEu
#define SHIZUKU_STREAM_ROLE_PRODUCER 0
#define SHIZUKU_STREAM_ROLE_CONSUMER 1

// Stream descriptor layout (stream.hpp に完全一致)
struct shizuku_stream_descriptor {
  void *base;            // データ領域の先頭 = REC buffer[capacity]
  uint32_t rec_size;     // 1 レコードのバイト数
  uint32_t capacity;     // レコード数
  uint32_t flags;        // SHIZUKU_STREAM_LOSSLESS 等
  volatile uint32_t wr;  // 公開済みレコード数 (producer が進める)
  volatile uint32_t rd;  // 消費済みレコード数 (consumer が進める)
  uint32_t producer;     // 席を取っているオブジェクト (NO_OWNER = 空き)
  uint32_t consumer;     // 席を取っているオブジェクト (NO_OWNER = 空き)
};

// SVC レジスタ規約 (ARM Cortex-M0+ / ARMv6-M)
// a0 = 番号, a1..a4 = 引数 / 戻り a0 = エラー (0 = 成功), a1 = 値
struct shizuku_syscall_result {
  uintptr_t error;
  uintptr_t value;
};

#if defined(__arm__) || defined(__thumb__)
static inline struct shizuku_syscall_result shizuku_svc(uintptr_t n, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
  register uintptr_t r0 __asm__("r0") = n;
  register uintptr_t r1 __asm__("r1") = a1;
  register uintptr_t r2 __asm__("r2") = a2;
  register uintptr_t r3 __asm__("r3") = a3;
  register uintptr_t r12 __asm__("r12") = a4;
  __asm__ volatile("svc 0"
                   : "+r"(r0), "+r"(r1)
                   : "r"(r2), "r"(r3), "r"(r12)
                   : "memory");
  struct shizuku_syscall_result res = {r0, r1};
  return res;
}
#else
static inline struct shizuku_syscall_result shizuku_svc(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  struct shizuku_syscall_result res = {0, 0};
  return res;
}
#endif

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_OBJECTLAND_ABI_H
