#include "tests/composition_link/sample_object.hpp"
#include "internal_headers/shizuku/object_id_decl.hpp"

// 全体ヘッダー (object_ids.hpp) への依存を一切持たず、
// 必要なシンボルのみを個別宣言することで、合成変更時の再コンパイルを回避する。
SHIZUKU_DECLARE_OBJECT_ID(sample_service)
SHIZUKU_DECLARE_OBJECT_ID(target_service)

// consumer 側で global reference alias を持っても動的初期化が発生しないことの検証
const uintptr_t &GLOBAL_SAMPLE_ALIAS = shizuku::object_id::sample_service;
inline constexpr const uintptr_t &GLOBAL_TARGET_ALIAS = shizuku::object_id::target_service;

namespace sample_module {

uintptr_t get_my_id() {
  return GLOBAL_SAMPLE_ALIAS;
}

uintptr_t get_target_id() {
  return GLOBAL_TARGET_ALIAS;
}

} // namespace sample_module
