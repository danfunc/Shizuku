#pragma once
#include <cstdint>

// canonical exported C ABI symbol を宣言し、
// C++ namespace では初期化不要な inline constexpr 参照 alias を提供する。
// オブジェクト実装は全体ヘッダー (object_ids.hpp) への依存を持たず、
// 参照したい ID のみ個別宣言することで合成変更による再コンパイルを回避する。
#define SHIZUKU_DECLARE_OBJECT_ID(name) \
  extern "C" const uintptr_t shizuku_object_id_##name; \
  namespace shizuku::object_id { \
    inline constexpr const uintptr_t &name = shizuku_object_id_##name; \
  }
