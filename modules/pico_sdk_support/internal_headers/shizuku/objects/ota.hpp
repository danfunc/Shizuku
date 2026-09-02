#ifndef SHIZUKU_OBJECTS_OTA_HPP
#define SHIZUKU_OBJECTS_OTA_HPP
#include "shizuku/objects/ble_uart.hpp"
#include "shizuku/objects/flash_map.hpp"
#include <cstdint>

// ===========================================================================
//  ota — 新しいファームウェアを BLE で受け取り、ステージング領域へ置く
// ===========================================================================
namespace shizuku {
namespace objects {
namespace ota {

// ステージング領域。★場所を決めているのは flash_map.hpp (重なりの検査つき)。
//   ここは**末尾側**に置く — bonding バンクの直下で終わり、上限は 1 本の
//   定数で守り切れる (伸びるのは flash FS のほうなので、境界の見張りが要る側を
//   境界から遠ざけてある)。
constexpr uint32_t STAGING_OFFSET = flash_map::STAGING_OFFSET;
constexpr uint32_t STAGING_BYTES = flash_map::STAGING_BYTES;

enum struct method : uintptr_t {
  MAIN = 0,
  // a0 = ble_uart の OTA 受信ストリーム番号。
  SET_INPUT_STREAM = 1,
  // 戻り値 = 進捗・結果を出す行ストリームの番号 (logger が購読する)。
  GET_STREAM = 2,
  POLL = 3,
  // 戻り値 = 現在の内部状態 (state 列挙値、IDLE=0)。★中継の入れ替え元
  //   (UART ブリッジ等) が「入力ストリームを引き渡してよいか」を判断する
  //   材料。ストリームの available()==0 は「積んだ分は pop された」しか
  //   意味せず、pop の中で走る feed()/finish_upload() (flash 書き込み・
  //   CRC・完了行の送出) が終わったことまでは保証しない。IDLE に戻るのを
  //   待って初めて「もう ota は何も出さない」と言える。
  GET_STATE = 4,
  // 戻り値 = 直近の転送が成功で終わったか (1=成功, 0=失敗)。★GET_STATE と
  //   違い reset_transfer() で消えない。DONE/FAILED から IDLE へ落ちる
  //   その一瞬しか GET_STATE では見えないので、判定を残す側 (ota 自身の
  //   スレッド) が確定させて持つ。ACK/NAK で相手へ伝える材料。
  GET_LAST_OK = 5,
  // 戻り値 = まだ受け取れていないチャンク数。★呼ぶと一覧を行で吐く
  //   ("NEED n=<k>" / "NEEDSEQ a,b,c" / "NEEDEND")。k==0 のときは
  //   そのまま flash の読み返し CRC まで走らせて done/mismatch を出す。
  //   ★これは XNOR (チャンク単位再送) 専用。従来の XNOZ 経路では 0 を返す。
  GET_MISSING = 6,
  // 戻り値 = 1 なら「ota は何も抱えていない」(メッセージの途中でもなく、
  //   feed() の最中でもない)。★中継 (UART ブリッジ) が baud を戻してよいかの
  //   判断に使う。GET_STATE==IDLE ではチャンク再送に使えない — ラウンドの
  //   合間の ota は IDLE ではなく CSEEK (次のチャンクを待っている) で待機して
  //   おり、ここで転送を捨てさせるわけにはいかないため。
  GET_QUIESCENT = 7,
  // 走りかけの転送を捨てて待ち受け (IDLE) へ戻す。
  // ★諦めた転送のあと ota は最大 2 分 CSEEK に居座る (チャンク再送の
  //   ラウンドの合間を守るための長いタイムアウト)。その間に次の転送を
  //   始めると **XNOR のファイルヘッダがチャンクデータとして食われる**ので、
  //   転送の頭でこれを撃って必ず待ち受けから始められるようにする。
  RESET = 8,
};

using frame_t = shizuku::objects::ble_uart::frame_t;

// オブジェクトを登録
// (任意のオブジェクトID、ステータス通知先LED/BlinkオブジェクトID、BLE切断要求先ble_uartオブジェクトID)
uint32_t register_ota(uintptr_t object_id, uintptr_t status_sink_obj_id = 0,
                      uintptr_t ble_uart_obj_id = 0);

// poll スレッドを起動
uint32_t start_ota(uintptr_t object_id);

// フラッシュの消去・書き込みの最中か。
//  ★★この間 **BLE へ何も流さないこと**。IRQ を止めている最中の BLE トラフィックは
//    CYW43 の SPI/PIO を壊し、板が電源断まで戻らなくなる (2026-08-30 に 3 回踏んだ)。
//    喋る側が自分から黙るしかない — 焼いている側は相手を止められない。
bool flash_busy();

} // namespace ota
} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_OTA_HPP
