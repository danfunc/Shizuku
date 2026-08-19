#ifndef SHIZUKU_KERNEL_ABI_HPP
#define SHIZUKU_KERNEL_ABI_HPP
#include <cstdint>

// ===========================================================================
//  カーネル ABI — ISA 非依存の定数と型 (docs/03_porting_policy.md D3)
// ===========================================================================
//  syscall の形 (Q1 暫定決定: 番号はレジスタ渡し。RISC-V の ecall に即値が無いため):
//    in : a0 = 番号, a1..a4 = 引数
//    out: a0 = エラーコード (0 = 成功), a1 = 値
//  ARMv8-M では a0..a4 = r0, r1, r2, r3, r12。
//
//  ★不変条件 I-1: カーネルは番号で経路を分岐しない。経路は「発行元が信頼された
//    活性化か」の 1 ビットだけで決まる。番号はその後のプリミティブ選択に使うだけ。
//  ★不変条件 I-2: CALL / SET_HANDLER を発行できるのは信頼された活性化だけ。
namespace shizuku {

enum struct primitive : uintptr_t {
  // a1 = call_request*。呼び出し元フレームを退避し、呼び先へ制御を移す。
  CALL = 1,
  // a1 = 落とす段数, a2 = 戻り値, a3 = エラーコード, a4 = ネスト数の申告
  // (0 = 申告しない)。申告が実際の深さと違えば 1 段も落とさずエラー (§9.3)。
  RETURN = 2,
  // a1 = **オブジェクトランド側の svc ハンドラ**の entry。信頼された活性化からのみ。
  // ★カーネルの svc ハンドラ (例外文脈で走る機構) とは別概念。ここで登録するのは
  //   カーネルオブジェクトが持つ、スレッドモードで走る方針側のハンドラ。
  SET_HANDLER = 3,
  // a1 = 対象スレッド ID。実行権のバトンパス (Phase 2b)。
  SWITCH = 4,
  // a1 = 対象スレッド ID, a2 = 期限 [µs]。時限実行権委譲 (Phase 2b)。
  GRANT = 5,
};

// CALL に渡す保護指定 (DESIGN §11 の軸 A/B)。将来ここへ region set を足す。
// ★静的な権限クラス (enum のリング) は作らない (D6)。ここにあるのは
//   「この活性化をどう走らせるか」であって「誰が偉いか」ではない。
constexpr uint32_t PROTECTION_PRIVILEGED = 0;
constexpr uint32_t PROTECTION_UNPRIVILEGED = 1u << 0; // 軸 A: 非特権で走らせる
constexpr uint32_t PROTECTION_TRUSTED = 1u << 1;      // カーネルプリミティブを許す

// CALL の引数一式。レジスタ 4 本に収まらないので単一アドレス空間の利点を使って
// ポインタで渡す (発行できるのは信頼された活性化だけなので偽装の余地は信頼境界の
// 内側にしかない = I-2)。カーネルは cookie を一切解釈しない (D1)。
struct call_request {
  uintptr_t entry_pc;      // 呼び先の入口
  uintptr_t callee_cookie; // 呼び先の「現在オブジェクト」(不透明)
  uintptr_t caller_cookie; // 呼び出し元 identity (不透明)。既定は元の発行元 = CALL_STRICT
  uint32_t protection;     // PROTECTION_* のビット和
  uintptr_t args[4];       // 呼び先が受け取る引数 (a0..a3)
};

// syscall の戻り (a0)。0 = 成功はワイヤ規約 (D12)。
//
// ★ここに並ぶのは「発行者が受け取り得る答え」だけ。カーネルは番号の意味を持たない
//   ので、「未知の番号」「権限がない」といった語彙はここに存在しない:
//   - 未知の番号: 信頼された活性化しかプリミティブ選択に到達しないので、番号が
//     存在しないのは**カーネル自身の不変条件の破れ** → 返さずに panic する (§14)。
//     信頼されていない活性化の svc は番号を見られることなくディスパッチャへ渡るので、
//     そもそも「未知」という判定が発生しない
//   - 権限がない: CALL / SET_HANDLER へ到達できるのが信頼された活性化だけ、という
//     経路そのものが I-2 を強制する。検査して弾く場面がない
enum struct kernel_error : uintptr_t {
  OK = 0,
  BAD_REQUEST,    // call_request が不正 (null / entry 0)
  NO_STACK,       // 呼び出しフレームを積むスタックが足りない (panic しない)
  DEPTH_MISMATCH, // 申告したネスト数が実際と違う (1 段も落としていない)
  BAD_COUNT,      // 落とす段数が 0、または現在の深さを超えている (同上)
  NOT_READY,      // 対象スレッドが READY でない / claim 競り負け (Phase 2b)
  BAD_AFFINITY,   // 対象スレッドがこのコアで走ることを許されていない (Phase 2b)
};

} // namespace shizuku
#endif // SHIZUKU_KERNEL_ABI_HPP
