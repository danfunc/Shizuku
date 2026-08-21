// ===========================================================================
//  オブジェクトランドの記憶 — 階級別空きリストによる O(1) の貸し借り
// ===========================================================================
//  ★なぜ first-fit をやめたか: svc ハンドラは定数時間で抜けるのに、その先の
//    方針側が「それまでの借り方」に依存して伸びるのでは意味が無い。最悪値が
//    読めなければ、実行権をどれだけ貸せばよいかの見積もりも立たない
//    (貸し借りをクロック基準にした D24 と同じ動機)。
//
//  【O(1) にするための道具立て】
//    (1) **階級別の空きリスト** — 大きさを 2 冪で階級に分け、階級ごとに空きを
//        繋ぐ。要求に対しては「その階級のどれを取っても足りる」階級 (= ceil で
//        取った階級) 以上から選ぶので、階級の中を歩かなくてよい
//    (2) **空き階級のビットマップ** — 「要求以上で最小の空き階級」を ctz 1 発で
//        引く。これが無いと階級の数だけ舐めることになる
//    (3) **境界タグ (prev_bytes)** — 解放時に左隣を O(1) で見つける。旧実装は
//        併合のためにリストを頭から歩いていて、そこが一番重かった
//    (4) **双方向リスト** — 併合で消えるブロックを途中から O(1) で外す
//
//  ★2 つの arena を同じ実装で扱う。違うのは置き場所だけ (簿記用は静的領域 =
//    非特権から届かない、オブジェクト用はヒープ = 届く)。所有と保護は別の話。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"

namespace shizuku {

using block = KERNEL_OBJECT::block;
using arena = KERNEL_OBJECT::arena;
static constexpr uintptr_t BLOCK_ALIGN = KERNEL_OBJECT::BLOCK_ALIGN;
static constexpr uint32_t FREE_CLASSES = KERNEL_OBJECT::FREE_CLASSES;
static constexpr uintptr_t NO_OBJECT = KERNEL_OBJECT::NO_OBJECT;
// これ未満しか余らないなら切り分けない (ヘッダだけの断片を作らない)。
static constexpr uintptr_t MIN_BLOCK = sizeof(block) + BLOCK_ALIGN;

static_assert(sizeof(block) % BLOCK_ALIGN == 0, "ヘッダも境界に乗せる");

// ★要求に使う階級は **ceil** で取る。floor だと「その階級には足りないものも
//   混じる」ので中を歩いて探すことになり、そこで定数時間が崩れる。
static uint32_t class_for_request(uintptr_t bytes) {
  if (bytes <= 1)
    return 0;
  const uint32_t ceil_log2 =
      32u - (uint32_t)__builtin_clz((uint32_t)(bytes - 1u));
  return ceil_log2 >= FREE_CLASSES ? FREE_CLASSES - 1u : ceil_log2;
}

// 空きを置く階級は **floor**。その階級は「2^c 以上」を保証する側なので、
// 実際の大きさ以下の階級に置かないと保証が嘘になる。
static uint32_t class_of_block(uintptr_t bytes) {
  const uint32_t log2 = 31u - (uint32_t)__builtin_clz((uint32_t)bytes);
  return log2 >= FREE_CLASSES ? FREE_CLASSES - 1u : log2;
}

static void free_push(arena &target, block *item) {
  const uint32_t cls = class_of_block(item->bytes);
  item->prev = nullptr;
  item->next = target.free_list[cls];
  if (item->next != nullptr)
    item->next->prev = item;
  target.free_list[cls] = item;
  target.free_map |= (1u << cls);
}

static void free_remove(arena &target, block *item) {
  const uint32_t cls = class_of_block(item->bytes);
  if (item->prev != nullptr)
    item->prev->next = item->next;
  else
    target.free_list[cls] = item->next;
  if (item->next != nullptr)
    item->next->prev = item->prev;
  if (target.free_list[cls] == nullptr)
    target.free_map &= ~(1u << cls);
  item->next = item->prev = nullptr;
}

// 物理的に右隣。arena の端なら nullptr。
static block *right_of(arena &target, block *item) {
  const uintptr_t next = (uintptr_t)item + item->bytes;
  return next >= target.base + target.bytes ? nullptr : (block *)next;
}
// 物理的に左隣。境界タグがあるので歩かずに引ける。
static block *left_of(block *item) {
  return item->prev_bytes == 0 ? nullptr
                               : (block *)((uintptr_t)item - item->prev_bytes);
}

template <>
void KERNEL_OBJECT::arena_init(arena &target, uintptr_t base, uintptr_t bytes) {
  const uintptr_t aligned = (base + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
  const uintptr_t usable = (bytes - (aligned - base)) & ~(BLOCK_ALIGN - 1);
  // ★最上位階級が飽和すると、そこだけ「中を歩く」ことになって定数時間が崩れる。
  //   飽和しない大きさであることを組み立ての時点で確かめる (気をつけるでは守れない)。
  if (usable >= ((uintptr_t)1 << (FREE_CLASSES - 1)))
    KERNEL::BOARD::panic("arena too large for the free-class table");
  if (usable < MIN_BLOCK)
    KERNEL::BOARD::panic("arena too small to hold a single block");
  target.base = aligned;
  target.bytes = usable;
  for (uint32_t cls = 0; cls < FREE_CLASSES; ++cls)
    target.free_list[cls] = nullptr;
  target.free_map = 0;

  block *first = (block *)aligned;
  first->bytes = usable;
  first->prev_bytes = 0;
  first->owner = (uint16_t)NO_OBJECT;
  first->used = 0;
  free_push(target, first);
}

// ★借りた相手 (owner) を必ず記録する。誰のものか分からない記憶は、返させることも
//   会計に載せることもできない — それは「資源を持つ」と言えない状態。
template <>
uintptr_t KERNEL_OBJECT::arena_allocate(arena &target, uintptr_t bytes,
                                        uintptr_t owner) {
  if (bytes == 0)
    return 0;
  uintptr_t need = (bytes + sizeof(block) + BLOCK_ALIGN - 1) &
                   ~(uintptr_t)(BLOCK_ALIGN - 1);
  if (need < MIN_BLOCK)
    need = MIN_BLOCK;
  // 「要求以上で最小の空き階級」を 1 命令で引く。階級を順に舐めない。
  const uint32_t wanted = class_for_request(need);
  const uint32_t available = target.free_map & ~((1u << wanted) - 1u);
  if (available == 0)
    return 0; // 足りる空きが無い
  const uint32_t cls = (uint32_t)__builtin_ctz(available);
  block *chosen = target.free_list[cls];
  free_remove(target, chosen);

  // 意味のある大きさが残るときだけ切り分ける (ヘッダだけの断片を作らない)。
  if (chosen->bytes >= need + MIN_BLOCK) {
    block *rest = (block *)((uintptr_t)chosen + need);
    rest->bytes = chosen->bytes - need;
    rest->prev_bytes = need;
    rest->owner = (uint16_t)NO_OBJECT;
    rest->used = 0;
    chosen->bytes = need;
    // 右隣から見た「左の大きさ」も直す (境界タグは両側で辻褄が合っていないと
    // 併合が別のブロックを踏む)。
    block *after = right_of(target, rest);
    if (after != nullptr)
      after->prev_bytes = rest->bytes;
    free_push(target, rest);
  }
  chosen->used = 1;
  chosen->owner = (uint16_t)owner;
  return (uintptr_t)chosen + sizeof(block);
}

// 返す。**隣とだけ**繋げ直す (旧実装のようにリスト全体は歩かない)。
template <> bool KERNEL_OBJECT::arena_release(arena &target, uintptr_t handle) {
  if (handle <= target.base || handle >= target.base + target.bytes)
    return false;
  block *item = (block *)(handle - sizeof(block));
  if (!item->used)
    return false; // 二重解放 / 取っ手が不正
  item->used = 0;
  item->owner = (uint16_t)NO_OBJECT;

  // 右と繋げる。消える側を空きリストから O(1) で外せるのが双方向リストの効能。
  block *right = right_of(target, item);
  if (right != nullptr && !right->used) {
    free_remove(target, right);
    item->bytes += right->bytes;
  }
  // 左と繋げる。境界タグがあるので探さずに辿れる。
  block *left = left_of(item);
  if (left != nullptr && !left->used) {
    free_remove(target, left);
    left->bytes += item->bytes;
    item = left;
  }
  block *after = right_of(target, item);
  if (after != nullptr)
    after->prev_bytes = item->bytes;
  free_push(target, item);
  return true;
}

} // namespace shizuku
