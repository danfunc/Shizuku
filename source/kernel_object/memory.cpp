// ===========================================================================
//  オブジェクトランドのメモリ管理 — 「持つ者が貸す」をメモリ軸へ (DESIGN §10.1)
// ===========================================================================
//  ★カーネルはメモリを持たない。誰にどれだけ渡すか、いつ返させるかは方針なので
//    ここが持つ (D1)。参照実装の「オブジェクトごとに 16 語」は簡易な一形態でしかなく、
//    それに縛られる必要はない — ここでは**大きさを指定して借り、持ち主を付け替えられる**
//    形にする。持ち主が付いていれば、オブジェクトが消えたときに何を返せるかも決まる
//    (DESIGN §4.1 ルール 3「自身が保有する資源を自由に開放できる」)。
//
//  ★arena は 2 つに分ける。**所有 (誰が用意するか) と保護 (どこに置くか) は別の話**:
//    - 簿記用: カーネルの簿記 (スレッド表・文脈) に貸す。**非特権から到達できない
//      場所**に無ければならないので静的領域に置く (region の外 = 特権のみ)
//    - オブジェクト用: オブジェクト自身が読み書きする。非特権から届く必要があるので
//      ヒープ (region1) から取る
//    どちらもこちらが用意して貸す点は同じで、違うのは置き場所だけ。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"

namespace shizuku {

template <> void KERNEL_OBJECT::arena_init(arena &target, uintptr_t base,
                                           uintptr_t bytes);
template <>
uintptr_t KERNEL_OBJECT::arena_allocate(arena &target, uintptr_t bytes,
                                        uintptr_t owner);
template <> bool KERNEL_OBJECT::arena_release(arena &target, uintptr_t handle);

// 空きブロックを 1 つ置いただけの状態にする。
template <>
void KERNEL_OBJECT::arena_init(arena &target, uintptr_t base, uintptr_t bytes) {
  target.base = (base + BLOCK_ALIGN - 1) & ~(uintptr_t)(BLOCK_ALIGN - 1);
  target.bytes = bytes - (target.base - base);
  block *first = (block *)target.base;
  first->bytes = target.bytes;
  first->owner = NO_OBJECT;
  first->used = 0;
  first->next = nullptr;
}

// first-fit。大きすぎる空きは切り分ける。
// ★借りた相手 (owner) を必ず記録する。誰のものか分からない記憶は、返させることも
//   会計に載せることもできない — それは「資源を持つ」と言えない状態。
template <>
uintptr_t KERNEL_OBJECT::arena_allocate(arena &target, uintptr_t bytes,
                                        uintptr_t owner) {
  const uintptr_t need =
      ((bytes + sizeof(block) + BLOCK_ALIGN - 1) & ~(uintptr_t)(BLOCK_ALIGN - 1));
  for (block *current = (block *)target.base; current != nullptr;
       current = current->next) {
    if (current->used || current->bytes < need)
      continue;
    // 切り分けても意味のある大きさが残るときだけ割る (断片を増やしすぎない)。
    if (current->bytes >= need + sizeof(block) + BLOCK_ALIGN) {
      block *rest = (block *)((uintptr_t)current + need);
      rest->bytes = current->bytes - need;
      rest->owner = NO_OBJECT;
      rest->used = 0;
      rest->next = current->next;
      current->bytes = need;
      current->next = rest;
    }
    current->used = 1;
    current->owner = (uint16_t)owner;
    return (uintptr_t)current + sizeof(block);
  }
  return 0;
}

// 返す。隣が空いていれば繋げ直す (返しただけで使えなくなるのを防ぐ)。
template <> bool KERNEL_OBJECT::arena_release(arena &target, uintptr_t handle) {
  if (handle <= target.base || handle >= target.base + target.bytes)
    return false;
  block *target_block = (block *)(handle - sizeof(block));
  if (!target_block->used)
    return false;
  target_block->used = 0;
  target_block->owner = NO_OBJECT;
  for (block *current = (block *)target.base; current != nullptr;
       current = current->next) {
    while (!current->used && current->next != nullptr && !current->next->used) {
      current->bytes += current->next->bytes;
      current->next = current->next->next;
    }
  }
  return true;
}

} // namespace shizuku
