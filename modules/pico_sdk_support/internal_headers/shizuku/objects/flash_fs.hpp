#ifndef SHIZUKU_OBJECTS_FLASH_FS_HPP
#define SHIZUKU_OBJECTS_FLASH_FS_HPP
#include <cstdint>
#include "shizuku/object_ids.hpp"
#include "shizuku/stream.hpp"

// ===========================================================================
//  flash FS オブジェクト — **XIP 前提**の、アドレスを返すファイル系
// ===========================================================================
//  ★FAT 系との違いはここ: FAT は「媒体はブロックの列で、読むとは buffer へ写す
//    ことだ」という前提で組まれている。RP2350 の flash は XIP で 0x10000000 に
//    そのまま見えているので、その前提は**この機械では嘘**になる。写す必要がない
//    ものを写す API は、写す分の時間と RAM を無条件に払わせる。
//
//    そこでこの FS は、引いた結果を**アドレスで**返す:
//      lookup("foo") → 0x103xxxxx, 1234 バイト
//    呼ぶ側はそのまま読める。写したければ写せばよいが、写さない選択ができる。
//    中身がコードなら、そのまま**その場で実行できる** (XIP なので)。
//
//  ★この設計の代償を隠さない: **配ったアドレスは動かせない**。だから削除は
//    「名前を空ける」だけで、空いた領域は詰め直さない (詰めるとアドレスが
//    ずれ、既に配った参照が全部腐る)。回収したければ FORMAT で全部捨てる。
//    ログや設定のように「たまに書いて、ずっと読む」ものに向く。
//
//  ★特権が要る理由 (= pico_sdk_support に置く理由):
//    消去・書き込みの間は **XIP そのものが止まる**。つまり flash 上のコードは
//    誰も実行できない — 割り込みも他のスレッドも動けない。ミリ秒単位で系全体が
//    止まるということなので、これは「たまたま特権命令を使う」のではなく、
//    **系を止める権利**を持つ操作。周期スレッドの隣で気軽に呼ぶものではない
//    (LED の揺らぎを測ったときと同じ話 — 誰かが長く握れば他が遅れる)。
//    読むほうは XIP なのでただのメモリ読み出しで、止まらないし特権も要らない。
namespace shizuku {
namespace objects {

// ★番号は書かない。ビルドシステムが振る (cmake/shizukuObjects.cmake, D28)。
//   以前はここに「埋まっている番号の表」を手で書いていたが、それは今日 2 回
//   事故った — 表を見て空きを探す作業そのものが事故の原因だった。
constexpr uintptr_t FLASH_FS_OBJECT = object_id::flashfs;

// メソッド番号。0 は main (生成側が据え、自分で残りを export する)。
enum struct flash_fs_method : uintptr_t {
  MAIN = 0,
  LOOKUP = 1, // a0 = flash_lookup*   戻り値 = XIP アドレス (0 = 無い)
  STORE = 2,  // a0 = flash_store*    戻り値 = 置かれた XIP アドレス (0 = 失敗)
  REMOVE = 3, // a0 = flash_lookup*   戻り値 = 1 = 消した
  LIST = 4,   // a0 = flash_entry*    戻り値 = 1 = その番号に居た
  FORMAT = 5, // 全部捨てて空き領域を取り戻す (配ったアドレスは全部腐る)
  STATUS = 6, // a0 = flash_status*   戻り値 = 使用バイト数
  // ★これから書く先を**先に消しておく**。消去は 1 セクタ約 33ms かかり、その間
  //   系全体が止まる — 締切のある系ではこれを書き込みの経路に置けない。
  //   置く前に空けておけば、書き込みは「まだ 0xFF の場所へ書くだけ」= 消去ゼロ。
  //   **いつ払うかを選べるようにする**のがこの API の役目 (費用は消えない)。
  PREPARE = 7, // a0 = flash_prepare*
  // ---- ストリーム経由の読み書き (制御はメソッド、データはストリーム) ----
  // ★メソッド呼び出しは ioctl 相当として残す。開け閉めは制御であって、
  //   1 レコードごとに svc を撃つ話ではない。
  // ★★**非特権オブジェクトから読み書きできる**ことが要点:
  //   - 読み: 運ぶのは extent (XIP アドレス + 長さ)。XIP は region0 が
  //     読み出し + 実行を全員に許しているので、**非特権のまま直接読める**。写さない
  //   - 書き: 非特権はフラッシュを焼けない。だから**バイトをストリームへ流し**、
  //     特権側の書き手が汲んで焼く。締切のある側は 33ms を待たされない (D34)
  //   ★ストリームの実体は**オブジェクト arena** (非特権から届く側) に置く。
  //     静的領域に置くと region の外 = 特権のみになり、非特権が push できない。
  OPEN_READ = 8,   // a0 = flash_open*  → 戻り値 = ストリーム番号 (extent が流れる)
  OPEN_WRITE = 9,  // a0 = flash_open*  → 戻り値 = ストリーム番号 (chunk を流す)
  CLOSE_WRITE = 10, // 書き終わりを告げ、目録に載せる
};

// 書きのストリームが運ぶ 1 片。★大きさを固定するのは、ストリームが固定長
//   レコードの列だから (可変長にすると場所の計算が剰余で出せなくなる)。
constexpr uint32_t FLASH_CHUNK_BYTES = 64;
struct flash_chunk {
  uint32_t bytes; // 実際に詰まっている量 (最後の 1 片は半端になる)
  uint8_t data[FLASH_CHUNK_BYTES];
};

struct flash_open {
  const char *name;
  uint32_t reserve; // OPEN_WRITE のとき: 見込みの最大バイト数
  uintptr_t stream; // [out] ストリーム番号
};

// 名前は媒体に焼くので長さを固定する (可変長にすると、名前を伸ばすたびに
// 目録を書き直す = 消去が要る。目録は 1 セクタで済ませたい)。
constexpr uintptr_t FLASH_NAME_BYTES = 24;

// 引く。見つかれば address と bytes が埋まる。**中身は写されない**。
struct flash_lookup {
  const char *name;
  uintptr_t address; // [out] そのまま読める XIP アドレス
  uint32_t bytes;    // [out] 実バイト数
};

// 置く。同じ名前が居れば置き換える (古い領域は捨てられ、戻らない)。
struct flash_store {
  const char *name;
  const void *data;
  uint32_t bytes;
  uintptr_t address; // [out] 置かれた XIP アドレス
};

// 目録を頭から辿る。
struct flash_entry {
  uint32_t index;               // [in]  0 から
  char name[FLASH_NAME_BYTES];  // [out]
  uintptr_t address;            // [out]
  uint32_t bytes;               // [out]
};

// 先に空けておく要求。
struct flash_prepare {
  uint32_t bytes;   // [in] 置き場所の先をどれだけ空けておくか
  uint32_t erased;  // [out] 実際に消したバイト数 (0 = 既に空いていた)
  uint32_t took_us; // [out] かかった時間 = 系が止まった時間
};

struct flash_status {
  uintptr_t region_address; // 領域の先頭 (XIP アドレス)
  uint32_t region_bytes;    // 領域の大きさ
  uint32_t used_bytes;      // 目録 + 置いたものが占めている量
  uint32_t free_bytes;      // まだ置ける量 (詰め直しはしないので、これが全て)
  uint32_t entries;         // 居るファイル数
};

// 生成して main を呼び、目録を読む (無ければ初期化する)。0 = 成功。
uint32_t register_flash_fs();

// 実機での確認。**電源を切っても残るか**を見るのが要点なので、既に置いてあれば
// 書かずに読むだけで済ませる (毎回焼くと寿命を削るし、「残っていた」ことの
// 証明にもならない)。0 = 成功、それ以外 = 失敗した検査の数。
uint32_t flash_fs_probe();

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_FLASH_FS_HPP
