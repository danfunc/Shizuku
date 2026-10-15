#pragma once
#include <stdint.h>

// canonical exported C ABI symbol を宣言し、
// C++ namespace では初期化不要な inline constexpr 参照 alias を提供する。
// C コンパイラからも直接インクルード可能。
#ifdef __cplusplus
#define SHIZUKU_DECLARE_OBJECT_ID(name) \
  extern "C" const uintptr_t shizuku_object_id_##name; \
  namespace shizuku::object_id { \
    inline constexpr const uintptr_t &name = shizuku_object_id_##name; \
  }
#else
#define SHIZUKU_DECLARE_OBJECT_ID(name) \
  extern const uintptr_t shizuku_object_id_##name;
#endif
