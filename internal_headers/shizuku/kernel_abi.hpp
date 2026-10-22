#ifndef SHIZUKU_KERNEL_ABI_HPP
#define SHIZUKU_KERNEL_ABI_HPP
#include <cstdint>

// ===========================================================================
//  カーネル ABI — 機構だけの語彙 (docs/record/kernel_v2_record.typ D1 / DESIGN §7)
// ===========================================================================
//  syscall の形: a0 = 番号, a1..a4 = 引数 / 戻り a0 = エラー (0 = 成功), a1 = 値。
//
//  ★経路は番号で決めない (I-1)。決めるのは**今走っているオブジェクトの種別**だけ:
//      PLAIN / HANDLER → 登録済み親ハンドラへの dispatch
//      登録済み KOBJ → KOBJ 本人が primitive を解釈
//    ★段数のパリティでは決めない (2026-09-05)。パリティは「ハンドラ =
//      カーネルオブジェクト」を仮定しており、**一般オブジェクトがハンドラを務める
//      構成 (マルチ ABI) で崩れる**。ID でも決めない — ID で決め打つと kobj を
//      差し替えることも多重化することもできなくなる。
//    種別を書けるのはカーネルだけ (遷移のたびに呼び出しフレームへ退避し、
//    戻るときに読み戻す) なので、オブジェクト側からの偽装もできない。
//
//  ★RETURN を解釈できるのは登録済み KOBJ だけ。dispatch 先 C 関数の第 5 引数は
//    その呼出し鎖が戻ってよい最低深さ (unwind floor)。RETURN の a4 は絶対戻り先 d、
//    a1 は count = 現在深さ - d。カーネルは floor <= d < 現在深さと差分を検査する。
namespace shizuku {

// カーネルのプリミティブ番号。**ハンドラの枠の中からしか実行されない**。
enum struct primitive : uintptr_t {
  // a1 = call_request*。呼び出し元フレームを退避し、呼び先へ制御を移す。
  CALL = 1,
  // a1 = 落とす段数, a2 = 戻り値, a3 = エラーコード, a4 = 絶対戻り先深さ d。
  // d が unwind floor 未満、現在深さ以上、または count と不整合なら何も pop しない。
  RETURN = 2,
  // a1 = 対象スレッド ID。GRANT(対象, 自分の持つ予算の全部) と同じ。
  // 外側に貸し手が居て期限があればその残りを、起点のスレッドや kernel object のように
  // 予算が無制限なら無制限を渡す (外側でクランプされるので延長にはならない)。
  // 渡した側は WAIT_GRANT で待ち、借り手の yield か期限切れで戻る。借り手が撃てば又貸し。
  // 貸し手へ返すには、貸し手の ID を指す (祖先返却) か YIELD (svc_dispatch が処理する)。
  SWITCH = 3,
  // a1 = 対象スレッド ID, a2 = 期限 [µs]。時限つきで実行権を貸す。
  // 期限が来る (強制回収) か、借り手が yield する (早期復帰) と戻る。
  // 戻り: a0 = kernel_error, a1 = grant_end (EXPIRED / YIELDED)。
  // ★期限は外側の期限でクランプされる (I-7)。借りた以上は又貸しできない。
  GRANT = 4,
};

// RETURN stubs mark their trap so KOBJ can translate the single-pop
// notification into the absolute depth immediately below its own dispatch frame.
constexpr uintptr_t RETURN_STUB_MARKER = 0xA55Au;

// ===========================================================================
//  オブジェクトの種別 (2026-09-05 に復活させた)
// ===========================================================================
//  ★カーネルが「今どちらが走っているか」を決めるための唯一の属性。
//    経緯: 第 2 世代ではオブジェクト構造体の 32bit の enum フィールドだったが、
//    削除されて「ID 0 = カーネルオブジェクト」という決め打ちで代用されていた。
//    それだと kobj を差し替えることも多重化することもできない (ID は名前であって
//    役ではない) ので、独立した値として戻した。
//  ★D1「カーネルはオブジェクトを知らない」との関係: カーネルが持つのは
//    **今走っている文脈の種別**だけで、どのオブジェクトがどの種別かという台帳は
//    持たない — それは呼び出しのたびにオブジェクトランドが call_request で申告する。
//    申告できるのはプリミティブを撃てる側 = HANDLER だけなので、これは
//    「ハンドラが役を配る」形であって、一般オブジェクトの自己申告ではない。
enum struct object_kind : uint32_t {
  // 一般オブジェクト。svc は登録済みハンドラへのメソッド呼び出しとして届く。
  PLAIN = 0,
  // 専用 svc ハンドラを務めるオブジェクト。
  HANDLER = 1,
  // カーネルオブジェクト。プリミティブを撃てるのはこれだけ (I-2)。
  // 外部の call_request からはこの種別を名乗ることはできない (偽装防止)。
  KERNEL_OBJECT = 2,
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
  uintptr_t entry_pc;  // 呼び先の入口
  uint32_t protection; // PROTECTION_* のビット和
  uintptr_t args[4];   // 呼び先が受け取る引数 (a0..a3)
  uint32_t callee_object = 0; // 呼び先のオブジェクトID
  // ★呼び先の**種別**。既定は PLAIN — ハンドラを務めるオブジェクトを呼ぶときだけ
  //   HANDLER を載せる。ここが「役」を運ぶ唯一の場所で、ID からは決めない。
  uint32_t callee_kind = (uint32_t)object_kind::PLAIN;
  uint32_t callee_generation = 0;
  // ★軸 B (DESIGN §11.3)。呼び先が特権命令なしに読める追加の窓
  //   (region0/1 の外)。0/0 = 窓なし。**呼び出し元ではなく呼び先の性質**として
  //   決まる値をここへ載せる (docs/record/kernel_v2_record.typ Q8)。
  uintptr_t region_base = 0;
  uintptr_t region_limit = 0;
  // ★解決済み親 handling object の情報 (thread コンテキストへ伝播)
  uint32_t parent_handler_object = 0;
  uintptr_t parent_handler_entry = 0;
};
// ★戻り口はカーネルが自分の戻り口 1 本を載せる (発行側は指定しない)。return stub は
//   RETURN_STUB_MARKER を a4 に載せる。PLAIN / HANDLER から出た場合、dispatch が a1 を
//   ledger の unwind floor に置換してオブジェクトランドへ通知する。KOBJ から出た場合は
//   自分の dispatch 1段を caller へ戻す。複数段 exit は明示 d と count を使う。

// ★カーネルの答えがオブジェクトへ直接返るとき (ハンドラへ届ける途中で失敗した等) は
//   この印を付ける。**番号空間が違うものを素通しすると、受け取った側が自分の語彙で
//   読んでしまう** — 例えば 2 はカーネルでは NO_STACK、オブジェクトランドでは
//   BAD_OBJECT で、黙って別の意味になる。印があれば「これは自分の語彙ではない」と
//   分かるので、取り違えではなく異常として扱える。
constexpr uintptr_t KERNEL_ERROR_MARK = 0x80000000u;

// プリミティブが返す答え。受け取るのはカーネルオブジェクトだけ。
// ★「未知の番号」「権限がない」という語彙はここに存在しない — 番号を解釈するのは
//   オブジェクトランドであり、権限は経路の有無で決まるので判定する場面が無い。
// GRANT がどう終わったか (a1 で返る)。
enum struct grant_end : uintptr_t {
  EXPIRED = 0, // 期限が来たので取り上げた
  YIELDED = 1, // 借り手が自分から返した (早期復帰)
};

enum struct kernel_error : uintptr_t {
  OK = 0,
  BAD_REQUEST,    // entry_pc が 0
  NO_STACK,       // 呼び出しフレームを積むスタックが足りない (panic しない)
  DEPTH_MISMATCH, // 戻り先 d が unwind floor 未満か現在深さ以上 (1 枚も落としていない)
  BAD_COUNT,      // count が 0、過大、または current_depth - d と不整合 (同上)
  NOT_READY,      // 対象スレッドが READY でない / claim 競り負け
  BAD_AFFINITY,   // 対象スレッドがこのコアで走ることを許されていない
  NO_THREAD,      // スレッド枠が尽きた
  NO_MEMORY,      // スレッドスタックが確保できない
  GRANT_DEPTH,    // 貸し出しのネストが深すぎる
  // ★残量が「タイマが正直に数えられる最小」を下回った。**貸さずに断る**。
  //   丸め上げて貸すと、借り手は貸された以上に走れてしまい、外側でクランプして
  //   いる意味 (I-7: 借りた以上は又貸しできない) が壊れる。少ししか残っていない
  //   ときに黙って多めに貸すより、断って呼び出し元に判断させるほうが正しい。
  GRANT_TOO_SMALL,
};

// Shared by the trap path and host tests. A valid unwind removes exactly the
// frames above target_depth and never crosses the actor's ledger-authored floor.
constexpr kernel_error validate_return_depth(uint32_t depth, uint32_t floor,
                                             uint32_t target_depth,
                                             uintptr_t count) {
  if (target_depth < floor || target_depth >= depth)
    return kernel_error::DEPTH_MISMATCH;
  if (count == 0 || count != depth - target_depth)
    return kernel_error::BAD_COUNT;
  return kernel_error::OK;
}

struct return_stub_request {
  uint32_t target_depth;
  uintptr_t count;
};

// A KOBJ C return stub returns only its current dispatch frame. Multi-frame
// exits use the explicit object API target depth and are validated separately.
constexpr return_stub_request resolve_return_stub(uint32_t depth) {
  return {depth ? depth - 1 : 0, depth ? 1u : 0u};
}

} // namespace shizuku
#endif // SHIZUKU_KERNEL_ABI_HPP
