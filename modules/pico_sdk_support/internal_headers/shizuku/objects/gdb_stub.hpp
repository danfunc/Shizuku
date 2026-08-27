#ifndef SHIZUKU_OBJECTS_GDB_STUB_HPP
#define SHIZUKU_OBJECTS_GDB_STUB_HPP
#include <cstdint>
#include "shizuku/object_ids.hpp"

// ===========================================================================
//  GDB stub — プローブ無しで止めて覗く (docs D40)
// ===========================================================================
//  ★halting debug (SWD) はコアごと止めるが、こちらは DebugMonitor なので
//    **止まるのは対象のスレッドだけ**。他は走り続ける (I-9)。
//  ★入出力は今 USB CDC を直に叩いているが、本来はストリーム経由にすべき。
//    そうなれば診断出力と GDB は 2 本のストリームになり、「診断を黙らせる旗」も
//    要らなくなる (gdb_stub.cpp の read_byte/write_byte だけの差し替えで済む)。
namespace shizuku {
namespace objects {

// ===========================================================================
//  ★★役を 2 つのオブジェクトに分ける (D49)
// ===========================================================================
//  分ける理由は 2 つあって、どちらも「分けないと直せない」形の話:
//
//  (1) **デバッグハードウェアはコアごとに独立している**。FPB (ブレークポイントの
//      比較器) も DEMCR (MON_EN) も、書けるのは**自分が走っているコアの分だけ**。
//      1 本のスレッドでは片方のコアにしか仕掛けられないので、対象が反対の
//      コアで走っていると**永久に止まらない** (実測で踏んだ: 対象を core0 固定の
//      blink にしたら continue しても止まらず、診断には [STRESS] が出続けて
//      いた = 何周も通過しているのに当たらない、という形で出た)。
//      → **コアごとに 1 本ずつ agent を置く**。
//
//  (2) **信用できない入力を舐める側から特権を外せる**。RSP のパーサは
//      ワイヤから来たバイト列をそのまま解釈する、この stub で一番大きく一番
//      危ない部分。今はそれが FPB とカーネルのデバッグ API を握ったまま
//      走っている。役を分ければ、**特権が要るのは agent の側だけ**になり、
//      server は「読む・書く・解釈する」しかしない小さな相手に閉じ込められる
//      (DESIGN §11.2 の分業。ペリフェラルオブジェクトが特権を引き受けて
//      その上のドライバを非特権のままにするのと同じ形)。
//      ★今はまだ server も特権のまま — CDC を直に叩いているため (D42 残の
//        「CDC をストリームとして扱う」が入ると、ここが本当に非特権にできる)。
//        **構造だけ先に分けてある**。確かめていないことを「できる」と書かない。
constexpr uintptr_t GDB_SERVER_OBJECT = object_id::gdb_stub;
// 自コアのデバッグハードウェアを面倒見る側。コアの数だけスレッドを起こす。
constexpr uintptr_t GDB_AGENT_OBJECT = object_id::gdb_agent;
// 覗かれる側。★デバッグのために特別なことは何もしていない普通のオブジェクト。
constexpr uintptr_t DEBUGGEE_OBJECT = object_id::debuggee;

// 覗かれる側 (debuggee) と stub を起こす。0 = 成功。
// ★external_target を渡すと、合成の debuggee を作らずそのスレッドを対象にする
//   (2026-08-24 デモ用: 実在するスレッド — 例えば LED を叩く blink — を
//   attach するだけで止められることを目で見るため)。既定は今まで通り
//   合成の debuggee (docs/05_handoff.md の `break debuggee_step` はこちら前提)。
constexpr uint32_t NO_EXTERNAL_TARGET = 0xFFFFFFFFu;
uint32_t start_gdb_stub(uint32_t external_target = NO_EXTERNAL_TARGET);

// ===========================================================================
//  ★転送をストリームに差し替える (D42 残の一歩目)
// ===========================================================================
//  ヘッダ冒頭に「本来はストリーム経由にすべき」と書いてあった話の実装。
//  **CDC 直の経路はそのまま残してある** — 動いているものを壊さないため。
//  `start_gdb_stub()` は今まで通り CDC。`start_gdb_stub_over_stream()` を
//  使うと、代わりに 2 本のストリームで喋る。
//
//  ★なぜバイトではなく「束」なのか: ストリームは 1 レコード = 1 回の
//    push/pop なので、1 バイト 1 レコードにすると RSP の 1 パケット
//    (数百バイト) で数百回まわることになる。束にして、読み出し側が
//    手元でバイトに崩す。
struct link_chunk {
  uint16_t len;
  uint8_t data[64];
};

//  転送側 (例: BLE) が繋ぐ 2 本。**向きは stub から見た名前**。
struct gdb_link {
  uintptr_t to_stub;   // 転送 → stub (host が書いたバイト)。転送側が producer。
  uintptr_t from_stub; // stub → 転送 (GDB への返事)。転送側が consumer。
  bool ok;
};

gdb_link start_gdb_stub_over_stream(uint32_t external_target = NO_EXTERNAL_TARGET);

//  ★転送側が「デバッガが繋がっているか」を教える。CDC なら
//    `usb_cdc_connected()` が答えていた所で、BLE には対応物が無いため
//    (notify が有効で、かつリンクが認可済み、が「繋がっている」の意味になる)。
//    **繋がっていない間 stub は寝る** (D48) ので、ここを立て忘れると
//    「GDB が繋がらない」ではなく「無反応」に見える。
void gdb_link_set_connected(bool connected);

//  ★★転送を回しているスレッドを申告する (D56)。**そのスレッドは
//    `monitor target` で止められなくなる** —— 止めるとデバッガ自身が
//    喋れなくなり、止めた本人が復旧できない (無線だと電源再投入まで戻らない)。
//  ★★★本来これは **System Object が持つべき制約** (誰が誰を止めてよいか)。
//    stub が自前の表で守るのは、資源の階層がまだ無いための繋ぎ。
//    入ったらそちらへ移すこと。
void gdb_link_protect_thread(uint32_t thread);

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_GDB_STUB_HPP
