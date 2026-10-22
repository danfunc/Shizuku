#ifndef SHIZUKU_ARCH_RISCV_ECALL_TABLE_HPP
#define SHIZUKU_ARCH_RISCV_ECALL_TABLE_HPP
#include <cstddef>
// M-mode ECALL を a0 の番号で直接振り分ける表 (shizuku_rv32_ecall_table) の検査。
// 表は chip の trap.cpp が持つ。要素 = 番号に対応する処理関数で、trap 内で直接処理する番号
// (svc_attributes の mode == handler) だけがその API 専用の関数、残りは全部 `other`
// (kobj への転送・primitive・objectland の raw 配送を従来どおり決める共通の入口)。
namespace shizuku::ports::riscv {
// 表と属性が食い違わないこと:
//   - 大きさが番号の数と同じ (asm が範囲外を 0 番へ落とす前提)
//   - 0 番は `other` (範囲外の番号の行き先)
//   - is_native(id) な番号だけが `other` 以外
template <class Table, class IsNative>
constexpr bool ecall_table_consistent(const Table &table, size_t entries,
                                      typename Table::value_type other,
                                      IsNative is_native) {
  if (table.size() != entries || entries == 0 || table[0] != other) return false;
  for (size_t id = 0; id < table.size(); ++id)
    if ((table[id] != other) != is_native(id)) return false;
  return true;
}
}
#endif
