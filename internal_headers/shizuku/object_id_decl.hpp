#pragma once
#include <cstdint>

// 合成非依存のオブジェクトID個別宣言用ヘッダー。
// 各オブジェクト／モジュールは、全体ヘッダー (object_ids.hpp) ではなく
// 自身が参照するシンボル名のみを extern const 宣言することで、
// 合成ごとの構成変更による再コンパイルを完全に回避する。
#define SHIZUKU_DECLARE_OBJECT_ID(name) \
  namespace shizuku::object_id { extern const uintptr_t name; }
