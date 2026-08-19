#ifndef SHIZUKU_KERNEL_ABI_HPP
#define SHIZUKU_KERNEL_ABI_HPP
#include <cstdint>

// ===========================================================================
//  カーネル ABI — 機構だけの語彙 (docs/03_porting_policy.md D1 / DESIGN §7)
// ===========================================================================
//  syscall の形: a0 = 番号, a1..a4 = 引数 / 戻り a0 = エラー (0 = 成功), a1 = 値。
//
//  ★経路は番号で決めない (I-1)。決めるのは**カーネル自身が積んだ呼び出しフレーム**
//    だけ: 今走っているコードが「カーネルがハンドラを起こすために積んだ枠」の中なら
//    プリミティブ、そうでなければ登録済みハンドラへのトランポリン。
//    呼び出しの履歴を書けるのはカーネルだけなので、この判定は偽装できない。
//    → カーネルは identity (cookie) も信頼ビットも持たない。
//
//  ★カーネルオブジェクト以外は巻き戻し (RETURN) を撃てない。だから:
//    (a) カーネルはハンドラを起こすとき**今のネスト数を渡す** (ARCH の活性化情報)
//    (b) オブジェクトは exit API に**何段戻すか**を載せて撃つ
//    (c) ハンドラがその段数で RETURN する。段数は必ず申告し、カーネルが実際の深さと
//        突き合わせる (§9.3 の両側チェック)
namespace shizuku {

// カーネルのプリミティブ番号。**ハンドラの枠の中からしか実行されない**。
enum struct primitive : uintptr_t {
  // a1 = call_request*。呼び出し元フレームを退避し、呼び先へ制御を移す。
  CALL = 1,
  // a1 = 落とす段数, a2 = 戻り値, a3 = エラーコード, a4 = ネスト数の申告。
  // 申告が実際の深さと違えば 1 枚も落とさずエラー。
  RETURN = 2,
  // a1 = 対象スレッド ID。実行権のバトンパス (Phase 2b)。
  SWITCH = 3,
  // a1 = 対象スレッド ID, a2 = 期限 [µs]。時限実行権委譲 (Phase 2b)。
  GRANT = 4,
};

// CALL に渡す保護指定 (DESIGN §11 の軸 A/B)。将来ここへ region set を足す。
// ★静的な権限クラス (enum のリング) は作らない (D6)。ここにあるのは
//   「この呼び出しをどう走らせるか」であって「誰が偉いか」ではない。
constexpr uint32_t PROTECTION_PRIVILEGED = 0;
constexpr uint32_t PROTECTION_UNPRIVILEGED = 1u << 0; // 軸 A: 非特権で走らせる

// 呼び出しの指定一式。カーネルはここに identity を持たない — 呼び出しを発行するのは
// 常にカーネルオブジェクトで、誰が誰を呼んだかはその台帳の側にある
// (PORT §3.1「呼び出し元 identity はカーネル支援不要」)。
struct call_request {
  uintptr_t entry_pc; // 呼び先の入口
  // 呼び先が普通に return したときの戻り口。呼び先は RETURN を撃てないので、
  // 「オブジェクトランドの exit API を撃つコード」を発行側が与える (D5)。
  // 0 は不正 (BAD_REQUEST) — 既定値で黙って壊れないようにする。
  uintptr_t return_pc;
  uint32_t protection; // PROTECTION_* のビット和
  uintptr_t args[4];   // 呼び先が受け取る引数 (a0..a3)
};

// プリミティブが返す答え。受け取るのはカーネルオブジェクトだけ。
// ★「未知の番号」「権限がない」という語彙はここに存在しない — 番号を解釈するのは
//   オブジェクトランドであり、権限は経路の有無で決まるので判定する場面が無い。
enum struct kernel_error : uintptr_t {
  OK = 0,
  BAD_REQUEST,    // entry_pc / return_pc が 0
  NO_STACK,       // 呼び出しフレームを積むスタックが足りない (panic しない)
  DEPTH_MISMATCH, // 申告したネスト数が実際と違う (1 枚も落としていない)
  BAD_COUNT,      // 落とす段数が 0、または現在の深さを超えている (同上)
  NOT_READY,      // 対象スレッドが READY でない / claim 競り負け (Phase 2b)
  BAD_AFFINITY,   // 対象スレッドがこのコアで走ることを許されていない (Phase 2b)
};

} // namespace shizuku
#endif // SHIZUKU_KERNEL_ABI_HPP
