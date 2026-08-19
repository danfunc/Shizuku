#include "shizuku/kernel.hpp"

// 呼び出したコアの初期化のみ行う。優先度レジスタ等は per-core banked なので、
// 他コアはそのコア自身の起動経路 (Phase 2 の core1 boot) で init を呼ぶこと。
template <> void shizuku::CPU_MANAGER::init() {
  BOARD::init(BOARD::core_num());
}
