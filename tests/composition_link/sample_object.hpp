#pragma once
#include <cstdint>

namespace sample_module {

// サンプルオブジェクトが自身のIDおよび通信相手のIDを読み取って返す関数
uintptr_t get_my_id();
uintptr_t get_target_id();

} // namespace sample_module
