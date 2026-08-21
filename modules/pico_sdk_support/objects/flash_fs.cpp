// ===========================================================================
//  flash FS オブジェクト — XIP 前提の、アドレスを返すファイル系 (実装)
// ===========================================================================
//  設計の理由は shizuku/objects/flash_fs.hpp を参照。ここは機械の都合を書く。
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/flash_fs.hpp"

// ★リンカが置いたファーム末尾。名前空間の中に書くと C++ のマングリングが効いて
//   別物になるので、必ずファイルスコープの extern "C" で受ける (同じ罠を
//   __end__ で一度踏んでいる)。
extern "C" char __flash_binary_end;

namespace shizuku {
namespace objects {
namespace {

using ARCH = KERNEL::ARCH;

// ★消去中は XIP が止まる = flash 上のコードを誰も実行できない。もう一方のコアが
//   その間に flash を踏むと即死するので、本来は相手を RAM 上へ退避させて止める
//   (pico-sdk の flash_safe_execute) 必要がある。今の構成は 1 コアなので割り込みを
//   落とすだけで足りる — が、「足りる理由」は構成に依存しているので、構成が
//   変わったらここで気づけるようにしておく。
// ★多コアでは割り込みを落とすだけでは足りない。消去中は XIP そのものが止まるので、
//   もう一方のコアが flash を踏んだ瞬間に死ぬ。BOARD::park_other_cores() が
//   相手を RAM 上のコードへ引き剥がして待たせる (board.cpp の注記を参照)。
//   ★止めるのは「行儀」ではなく機械の要求。そして止めている間、相手のコアは
//     何も進まない — **書き込みは両方のコアを止める操作**。

struct call_result {
  uintptr_t error;
  uintptr_t value;
};

call_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uintptr_t export_method(uintptr_t method, uintptr_t entry) {
  return api(object_api::EXPORT_METHOD, method, entry).error;
}

// ---- 媒体の割り付け --------------------------------------------------------
//  flash の**末尾**を切り出して使う。ファームは先頭から伸びるので、末尾を取る限り
//  「大きくなったファームに踏まれる」ことが無い。踏みそうなときは mount で気づく。
constexpr uint32_t REGION_BYTES = 1024 * 1024;
constexpr uint32_t REGION_OFFSET = PICO_FLASH_SIZE_BYTES - REGION_BYTES;
constexpr uintptr_t REGION_ADDRESS = XIP_BASE + REGION_OFFSET;

constexpr uint32_t DIRECTORY_MAGIC = 0x5A4B4653; // 'ZKFS'
constexpr uint32_t DIRECTORY_VERSION = 2;

// ===========================================================================
//  目録は**追記式** (2026-08-21 の実測を受けて書き直した)
// ===========================================================================
//  ★実測: 41 バイトのファイル 1 つを書くのに消去が 65,697µs (全体の 89%) かかって
//    いた。内訳は 4KB セクタの消去 × 2 回で、1 回あたり約 33ms。33ms は W25Q の
//    物理的な限界 (typ 45ms なのでむしろ速い側) なので、**媒体を責める話ではなく
//    「要らない消去を 2 回している」という手順の話**だった。
//
//  ★NOR フラッシュは**書き込みが 1→0 しかしない**。消去が 0xFF に戻す操作で、
//    書き込みはビットを落とすだけ。したがって:
//      - **まだ 0xFF の場所へは、消さずに書ける**
//      - 状態の遷移も「ビットを落とす向き」なら消さずに書ける
//    この 2 つを使うと、普通の store から消去が丸ごと消える。
//
//  【目録の形】ヘッダ 1 枠 + 追記される項目。**書き換えない**のが要点:
//    枠 0      : 名乗り (magic/version)。セクタを消した直後に 1 度だけ書く
//    枠 1..127 : 項目。空き (0xFF) の枠へ**追記**する。書き換えは一切しない
//  - 置き場所の先頭 (bump) は**持たない**。項目から導出する — 持つと store の
//    たびに書き換えが要り、書き換えには消去が要る (それが 2 回目の消去だった)
//  - 削除は state の**ビットを落とす**だけ (LIVE → DEAD)。0xFF へ戻さないので
//    消去が要らない
//  - 同じ名前を置き直すときも、古い項目を DEAD にして新しい項目を追記する
struct directory_entry {
  uint32_t state; // FREE(全 1) → LIVE → DEAD。ビットを落とす向きにしか動かない
  uint32_t address;
  uint32_t bytes;
  char name[FLASH_NAME_BYTES - 4]; // 32 バイトに収める
};
static_assert(sizeof(directory_entry) == 32, "1 枠 32 バイト");

constexpr uint32_t ENTRY_FREE = 0xFFFFFFFFu;
constexpr uint32_t ENTRY_LIVE = 0xFFFFFFFEu;
constexpr uint32_t ENTRY_DEAD = 0xFFFFFFFCu;
constexpr uint32_t ENTRY_COUNT = FLASH_SECTOR_SIZE / sizeof(directory_entry);
constexpr uint32_t FIRST_ENTRY = 1; // 枠 0 は名乗り

// 目録の作業コピー。**RAM に無いといけない**理由は 2 つ:
//   (1) 書き込み中は XIP が止まるので、書き込み元が flash 上にあると読めない
//   (2) 引くたびに flash を舐めると、XIP キャッシュを汚して他が遅くなる
directory_entry g_directory[ENTRY_COUNT];
bool g_mounted = false;

// 書き込み元を 1 ページずつ RAM へ写すための中継。**XIP が止まっている間に
// 呼び出し側のバッファ (文字列リテラルなら flash 上にある) を読むことはできない**
// ので、写してから渡す。
alignas(4) uint8_t g_page[FLASH_PAGE_SIZE];

constexpr uint32_t sector_round_up(uint32_t bytes) {
  return (bytes + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
}

bool name_equal(const char *left, const char *right) {
  for (uintptr_t index = 0; index < sizeof(directory_entry::name); ++index) {
    if (left[index] != right[index])
      return false;
    if (left[index] == '\0')
      return true;
  }
  return true;
}

void name_copy(char *destination, const char *source) {
  uintptr_t index = 0;
  const uintptr_t limit = sizeof(directory_entry::name);
  for (; index + 1 < limit && source[index] != '\0'; ++index)
    destination[index] = source[index];
  for (; index < limit; ++index)
    destination[index] = '\0';
}

int find_entry(const char *name) {
  for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index) {
    if (g_directory[index].state != ENTRY_LIVE)
      continue;
    if (name_equal(g_directory[index].name, name))
      return (int)index;
  }
  return -1;
}

// 置き場所の先頭は**項目から導出する**。持たない理由は上の注記のとおり。
uint32_t derive_bump() {
  uint32_t bump = (uint32_t)(REGION_ADDRESS + FLASH_SECTOR_SIZE);
  for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index) {
    // ★DEAD も数える。**領域は返らない** — 配ったアドレスを動かせないので
    //   詰め直しができない (D21)。死んだ項目の場所を再利用すると、まだそのアドレスを
    //   持っている誰かの足元を書き換えることになる。
    if (g_directory[index].state == ENTRY_FREE)
      continue;
    const uint32_t end =
        g_directory[index].address + sector_round_up(g_directory[index].bytes);
    if (end > bump)
      bump = end;
  }
  return bump;
}

uint32_t live_count() {
  uint32_t live = 0;
  for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index)
    if (g_directory[index].state == ENTRY_LIVE)
      ++live;
  return live;
}

// ★まだ 0xFF か。0xFF なら**消さずに書ける** (書き込みはビットを落とすだけ)。
//   読むのは XIP なのでただのメモリ走査 = 消去 33ms に比べれば無視できる。
bool is_blank(uint32_t address, uint32_t bytes) {
  const uint32_t *word = (const uint32_t *)address;
  for (uint32_t index = 0; index < bytes / 4; ++index)
    if (word[index] != 0xFFFFFFFFu)
      return false;
  return true;
}

// ---- 媒体へ書く ------------------------------------------------------------
// ★ここが「系を止める」区間。**両方のコア**を止め、割り込みを落としてから消去・
//   書き込みを行い、必ず戻す。pico-sdk の flash_range_* は RAM 上で走る関数として
//   置かれており、終わりに XIP のキャッシュも捨ててくれる (捨てないと、書いた
//   直後に読んだとき古い中身が見える)。
// ★内訳を測る。「遅い」だけでは、媒体の限界なのか手順が悪いのかを言い分けられない。
uint64_t g_erase_us = 0;
uint64_t g_program_us = 0;
uint32_t g_erase_count = 0;
uint32_t g_program_count = 0;
uint32_t g_erased_bytes = 0;

// ★論理的な 1 操作 (store / remove / format) を丸ごと囲むための入れ物。
//   囲まなくても flash_write が個別に止めるので**正しさは変わらない** — 変わるのは
//   止め直す回数だけ (1 回の store で 3 回 → 1 回)。正しさを「気をつけて囲むこと」に
//   依存させないために、入れ子で数える形にしてある (board.cpp の注記)。
struct stop_the_world {
  stop_the_world() { KERNEL::BOARD::park_other_cores(); }
  ~stop_the_world() { KERNEL::BOARD::resume_other_cores(); }
  stop_the_world(const stop_the_world &) = delete;
  stop_the_world &operator=(const stop_the_world &) = delete;
};

void flash_write(uint32_t offset, const uint8_t *ram_data, uint32_t bytes,
                 bool erase_first, uint32_t erase_bytes) {
  // ★順序が大事: **先に相手を止めてから**自分の割り込みを落とす。逆にすると、
  //   相手を止めるための往復 (FIFO と応答待ち) が割り込みを落とした状態で走る
  //   ことになり、応答が来ないまま固まり得る。
  KERNEL::BOARD::park_other_cores();
  const uint32_t interrupts = save_and_disable_interrupts();
  if (erase_first) {
    const uint64_t began = ::time_us_64();
    ::flash_range_erase(offset, erase_bytes);
    g_erase_us += ::time_us_64() - began;
    ++g_erase_count;
    g_erased_bytes += erase_bytes;
  }
  if (bytes != 0) {
    const uint64_t began = ::time_us_64();
    ::flash_range_program(offset, ram_data, bytes);
    g_program_us += ::time_us_64() - began;
    ++g_program_count;
  }
  restore_interrupts(interrupts);
  KERNEL::BOARD::resume_other_cores();
}

// 目録の 1 枠を焼く。★ページ単位でしか書けないので、その枠を含む 256 バイトを
//   組み立てて書く。**同じページの他の枠には 0xFF を書く** — 0xFF は「ビットを
//   1 つも落とさない」ので、既に焼かれている隣の枠は変わらない。これが
//   「書き換えずに追記する」の実際の姿。
void write_entry(uint32_t index, const directory_entry &value) {
  const uint32_t byte_offset = index * sizeof(directory_entry);
  const uint32_t page = byte_offset / FLASH_PAGE_SIZE;
  const uint32_t within = byte_offset % FLASH_PAGE_SIZE;
  for (uint32_t at = 0; at < FLASH_PAGE_SIZE; ++at)
    g_page[at] = 0xFF;
  const uint8_t *source = (const uint8_t *)&value;
  for (uint32_t at = 0; at < sizeof(directory_entry); ++at)
    g_page[within + at] = source[at];
  flash_write(REGION_OFFSET + page * FLASH_PAGE_SIZE, g_page, FLASH_PAGE_SIZE,
              false, 0);
  g_directory[index] = value;
}

// 空いている枠を探す。★追記なので「一度使った枠は二度と空かない」。埋まったら
//   目録だけ詰め直す (中身のアドレスは動かないので、これは安全な操作)。
int free_slot() {
  for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index)
    if (g_directory[index].state == ENTRY_FREE)
      return (int)index;
  return -1;
}

// 目録セクタを消して、名乗りと生きている項目だけを書き直す。
// ★これは消去を伴う唯一の目録操作で、枠が尽きたときと FORMAT のときだけ走る。
void directory_rebuild(bool keep_live) {
  stop_the_world stopped; // 消去 + 書き直しをまとめて 1 回の停止で済ませる
  directory_entry saved[ENTRY_COUNT];
  uint32_t kept = 0;
  if (keep_live)
    for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index)
      if (g_directory[index].state == ENTRY_LIVE)
        saved[kept++] = g_directory[index];

  flash_write(REGION_OFFSET, nullptr, 0, true, FLASH_SECTOR_SIZE);
  for (uint32_t index = 0; index < ENTRY_COUNT; ++index) {
    g_directory[index].state = ENTRY_FREE;
    g_directory[index].address = 0xFFFFFFFFu;
    g_directory[index].bytes = 0xFFFFFFFFu;
  }
  directory_entry header{};
  header.state = DIRECTORY_MAGIC;
  header.address = DIRECTORY_VERSION;
  header.bytes = 0xFFFFFFFFu;
  for (uintptr_t at = 0; at < sizeof(header.name); ++at)
    header.name[at] = (char)0xFF;
  write_entry(0, header);
  for (uint32_t index = 0; index < kept; ++index)
    write_entry(FIRST_ENTRY + index, saved[index]);
}

// ---- メソッド --------------------------------------------------------------
uintptr_t flash_lookup_method(uintptr_t argument, uintptr_t, uintptr_t,
                              uintptr_t) {
  flash_lookup *request = (flash_lookup *)argument;
  if (request == nullptr || request->name == nullptr || !g_mounted)
    return 0;
  const int slot = find_entry(request->name);
  if (slot < 0) {
    request->address = 0;
    request->bytes = 0;
    return 0;
  }
  // ★写さない。返すのはアドレスそのもので、呼ぶ側はここから直接読む。
  //   XIP 領域は MPU 上も読み出し + 実行が許してあるので、**非特権の呼び出し元でも
  //   この先は自分で読める** (引くところだけが特権)。
  request->address = g_directory[slot].address;
  request->bytes = g_directory[slot].bytes;
  return request->address;
}

uintptr_t flash_store_method(uintptr_t argument, uintptr_t, uintptr_t,
                             uintptr_t) {
  flash_store *request = (flash_store *)argument;
  if (request == nullptr || request->name == nullptr || !g_mounted)
    return 0;
  if (request->data == nullptr || request->bytes == 0)
    return 0;

  // ★1 回の store は「中身のページ + 古い項目を殺す + 新項目」で最大 3 回書く。
  //   ここで囲むと止めるのは 1 回で済む (囲まなくても正しいが、止め直す往復が
  //   そのぶん減る)。
  stop_the_world stopped;
  const uint32_t reserved = sector_round_up(request->bytes);
  const uint32_t bump = derive_bump();
  if (bump + reserved > REGION_ADDRESS + REGION_BYTES)
    return 0; // 空きが無い。詰め直しはしない (アドレスが動くので)

  int slot = free_slot();
  if (slot < 0) {
    // 枠が尽きた。目録だけ詰め直す — **中身のアドレスは動かないので安全**
    // (動かせないのは中身のほうで、目録は動かしてよい)。
    directory_rebuild(true);
    slot = free_slot();
    if (slot < 0)
      return 0;
  }

  const uint32_t offset = bump - XIP_BASE;
  // ★★まだ 0xFF なら**消さない**。書き込みはビットを落とすだけなので、消去は
  //   「1 へ戻したいとき」にしか要らない。実測ではこの消去 1 回が 33ms で、
  //   store 全体の大半を占めていた。読んで確かめる費用は XIP のメモリ走査だけ。
  if (!is_blank(bump, reserved))
    flash_write(offset, nullptr, 0, true, reserved);

  // 中身を 1 ページずつ写しながら焼く。**呼び出し側のバッファが flash 上にある
  // かもしれない**ので、直接渡さずに必ず RAM を経由する (XIP が止まっている間は
  // flash が読めない = 書き込み元が消える)。
  const uint8_t *source = (const uint8_t *)request->data;
  for (uint32_t written = 0; written < request->bytes;
       written += FLASH_PAGE_SIZE) {
    const uint32_t chunk = (request->bytes - written < FLASH_PAGE_SIZE)
                               ? (request->bytes - written)
                               : FLASH_PAGE_SIZE;
    for (uint32_t index = 0; index < FLASH_PAGE_SIZE; ++index)
      g_page[index] = index < chunk ? source[written + index] : 0xFF;
    flash_write(offset + written, g_page, FLASH_PAGE_SIZE, false, 0);
  }

  // 同じ名前が既に居たら、その項目を**殺してから**新しい項目を追記する。
  // 殺すのはビットを落とすだけなので消去が要らない。
  const int previous = find_entry(request->name);
  if (previous >= 0) {
    directory_entry dead = g_directory[previous];
    dead.state = ENTRY_DEAD;
    write_entry((uint32_t)previous, dead);
  }
  directory_entry fresh{};
  fresh.state = ENTRY_LIVE;
  fresh.address = bump;
  fresh.bytes = request->bytes;
  name_copy(fresh.name, request->name);
  write_entry((uint32_t)slot, fresh);

  request->address = bump;
  return bump;
}

uintptr_t flash_remove_method(uintptr_t argument, uintptr_t, uintptr_t,
                              uintptr_t) {
  flash_lookup *request = (flash_lookup *)argument;
  if (request == nullptr || request->name == nullptr || !g_mounted)
    return 0;
  const int slot = find_entry(request->name);
  if (slot < 0)
    return 0;
  // ★消えるのは**名前だけ**。領域は返らない。返すには後ろを詰めるしかなく、
  //   詰めればアドレスが動く — アドレスこそがこの FS の API なので、動かせない。
  //   ★殺すのはビットを落とす向きの遷移なので、消去は要らない。
  stop_the_world stopped;
  directory_entry dead = g_directory[slot];
  dead.state = ENTRY_DEAD;
  write_entry((uint32_t)slot, dead);
  return 1;
}

uintptr_t flash_list_method(uintptr_t argument, uintptr_t, uintptr_t,
                            uintptr_t) {
  flash_entry *request = (flash_entry *)argument;
  if (request == nullptr || !g_mounted || request->index >= ENTRY_COUNT)
    return 0;
  uint32_t seen = 0;
  for (uint32_t index = FIRST_ENTRY; index < ENTRY_COUNT; ++index) {
    if (g_directory[index].state != ENTRY_LIVE)
      continue;
    if (seen++ != request->index)
      continue;
    name_copy(request->name, g_directory[index].name);
    request->address = g_directory[index].address;
    request->bytes = g_directory[index].bytes;
    return 1;
  }
  return 0;
}

uintptr_t flash_format_method(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★中身のセクタは消さない。消すと 1MB ぶんで 8 秒以上かかる (256 セクタ ×
  //   33ms)。代わりに、置くときに「まだ 0xFF か」を見て要るときだけ消す
  //   (遅延消去)。FORMAT が速いのはそのため。
  directory_rebuild(false);
  g_mounted = true;
  return 1;
}

uintptr_t flash_status_method(uintptr_t argument, uintptr_t, uintptr_t,
                              uintptr_t) {
  const uint32_t used = derive_bump() - (uint32_t)REGION_ADDRESS;
  flash_status *request = (flash_status *)argument;
  if (request != nullptr) {
    request->region_address = REGION_ADDRESS;
    request->region_bytes = REGION_BYTES;
    request->used_bytes = used;
    request->free_bytes = REGION_BYTES - used;
    request->entries = live_count();
  }
  return used;
}

// ---- mount ----------------------------------------------------------------
// 目録を XIP からそのまま読み、名乗りが合わなければ作り直す。**空の flash は
// 0xFF なので、初期化済みかどうかは名乗りで判る** (0 埋めを期待すると、消去直後の
// 媒体を誤って読む)。
uintptr_t flash_fs_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const directory_entry *media = (const directory_entry *)REGION_ADDRESS;
  bool sane = media[0].state == DIRECTORY_MAGIC &&
              media[0].address == DIRECTORY_VERSION;
  if (sane) {
    for (uint32_t index = 0; index < ENTRY_COUNT; ++index)
      g_directory[index] = media[index];
    // ★焼かれているのは**絶対アドレス**なので、他所で作った像を読むと外を指す
    //   ポインタを配ってしまう。範囲に収まっているかをここで確かめ、収まって
    //   いなければ知らない媒体として扱う (黙って配らない)。
    for (uint32_t index = FIRST_ENTRY; sane && index < ENTRY_COUNT; ++index) {
      const directory_entry &entry = g_directory[index];
      if (entry.state != ENTRY_LIVE)
        continue;
      if (entry.address < REGION_ADDRESS + FLASH_SECTOR_SIZE ||
          (uint64_t)entry.address + entry.bytes > REGION_ADDRESS + REGION_BYTES)
        sane = false;
    }
  }
  if (!sane)
    directory_rebuild(false); // まだ何も焼かれていない (あるいは別物)
  g_mounted = true;

  uintptr_t failures = api(object_api::DECLARE_NAME, (uintptr_t) "flashfs").error;
  failures += export_method((uintptr_t)flash_fs_method::LOOKUP,
                            (uintptr_t)&flash_lookup_method);
  failures += export_method((uintptr_t)flash_fs_method::STORE,
                            (uintptr_t)&flash_store_method);
  failures += export_method((uintptr_t)flash_fs_method::REMOVE,
                            (uintptr_t)&flash_remove_method);
  failures += export_method((uintptr_t)flash_fs_method::LIST,
                            (uintptr_t)&flash_list_method);
  failures += export_method((uintptr_t)flash_fs_method::FORMAT,
                            (uintptr_t)&flash_format_method);
  failures += export_method((uintptr_t)flash_fs_method::STATUS,
                            (uintptr_t)&flash_status_method);
  return failures;
}

} // namespace

uint32_t register_flash_fs() {
  // ★ファームが領域まで伸びていたら、書いた瞬間に自分を消す。焼く前に気づける
  //   唯一の場所なので、ここで止める (「気をつける」では守れない類の話)。
  if ((uintptr_t)&__flash_binary_end > REGION_ADDRESS) {
    KERNEL::BOARD::diag_printf(
        "[FLASHFS] firmware ends at %p, past the region at %p — refusing\n",
        (void *)&__flash_binary_end, (void *)REGION_ADDRESS);
    return 1;
  }

  const call_result created =
      api(object_api::CREATE_OBJECT, FLASH_FS_OBJECT,
          (uintptr_t)&flash_fs_main, OBJECT_PRIVILEGED);
  const call_result started =
      api(object_api::CALL_METHOD, FLASH_FS_OBJECT, 0, 0);
  if (created.error != 0 || started.error != 0 || started.value != 0) {
    KERNEL::BOARD::diag_printf(
        "[FLASHFS] FAILED: create=%lu call=%lu exports_failed=%lu\n",
        (unsigned long)created.error, (unsigned long)started.error,
        (unsigned long)started.value);
    return 1;
  }
  flash_status status{};
  api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
      (uintptr_t)flash_fs_method::STATUS, (uintptr_t)&status);
  KERNEL::BOARD::diag_printf(
      "[FLASHFS] ready (object %lu) at %p, %lu KiB, %lu files, %lu KiB free\n",
      (unsigned long)FLASH_FS_OBJECT, (void *)status.region_address,
      (unsigned long)(status.region_bytes / 1024),
      (unsigned long)status.entries,
      (unsigned long)(status.free_bytes / 1024));
  return 0;
}

// ---- 実機での確認 ----------------------------------------------------------
//  見たいのは 2 つ:
//    (1) 引いた**アドレスをそのまま読める**か (写していないことの確認)
//    (2) 電源を切っても残るか
//  (2) は「今書いて今読む」では確かめられない。なので、既に置いてあるときは
//  **書かずに読むだけ**にして、2 回目以降の起動が前回の書き込みを見ていることを
//  そのまま証拠にする (ついでに flash の寿命も削らない)。
// 1 回置いて、内訳を印字する。★消去の回数を出すのが要点 — 「速い/遅い」ではなく
//   「消したかどうか」で説明がつくことを見えるようにする。
static uint32_t store_and_report(const char *name, const char *payload,
                                 uint32_t bytes, uintptr_t &address) {
  g_erase_us = g_program_us = 0;
  g_erase_count = g_program_count = g_erased_bytes = 0;
  flash_store store{name, payload, bytes, 0};
  const uint64_t began = KERNEL::BOARD::time_us();
  api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
      (uintptr_t)flash_fs_method::STORE, (uintptr_t)&store);
  const uint64_t took = KERNEL::BOARD::time_us() - began;
  address = store.address;
  if (store.address == 0) {
    KERNEL::BOARD::diag_printf("[FLASHFS] store '%s' failed\n", name);
    return 1;
  }
  KERNEL::BOARD::diag_printf(
      "[FLASHFS] stored '%s' at %p in %lu us "
      "(erase %lu us x%lu, program %lu us x%lu)\n",
      name, (void *)store.address, (unsigned long)took,
      (unsigned long)g_erase_us, (unsigned long)g_erase_count,
      (unsigned long)g_program_us, (unsigned long)g_program_count);
  return 0;
}

uint32_t flash_fs_probe() {
  static const char PAYLOAD[] = "shizuku: an object is a thing that runs.";
  constexpr uint32_t PAYLOAD_BYTES = sizeof(PAYLOAD);
  uint32_t failures = 0;

  // (1) 前回焼いたものが残っているか。★これは「今書いて今読む」では確かめられない
  //     ので、**書く前に**見る。
  flash_lookup lookup{"probe.txt", 0, 0};
  api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
      (uintptr_t)flash_fs_method::LOOKUP, (uintptr_t)&lookup);
  bool survived = lookup.address != 0 && lookup.bytes == PAYLOAD_BYTES;
  if (survived) {
    const char *previous = (const char *)lookup.address;
    for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
      if (previous[index] != PAYLOAD[index]) {
        survived = false;
        break;
      }
  }
  KERNEL::BOARD::diag_printf(
      "[FLASHFS] probe.txt from the previous boot: %s\n",
      survived ? "found and matches" : "absent or different");

  // (2) ★**前へ進み続ける**。同じ名前で置き直しても、古い項目を殺して新しい場所へ
  //     書くだけなので、行き先はまだ 0xFF = 消去が要らない。ここで毎回 FORMAT すると
  //     行き先が前回使ったセクタへ巻き戻り、追記式にした意味が消える (実測で
  //     そうなった: 毎回 33ms の消去が復活した)。
  //     ★これは多コアの試験でもある: 消去中は XIP が止まるので、もう一方のコアが
  //       止められていなければそのコアは flash 上のコードを踏んで即死する。
  uintptr_t first = 0;
  failures += store_and_report("probe.txt", PAYLOAD, PAYLOAD_BYTES, first);
  if (first == 0) {
    // 空きが尽きた。★ここが「詰め直さない」設計の代償を払う場所 —
    //   配ったアドレスを動かせない以上、まとめて捨てる以外に回収の道が無い (D21)。
    KERNEL::BOARD::diag_printf("[FLASHFS] region full, formatting\n");
    api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
        (uintptr_t)flash_fs_method::FORMAT, 0);
    failures = store_and_report("probe.txt", PAYLOAD, PAYLOAD_BYTES, first);
    if (first == 0)
      return failures;
  }

  // (4) 引いたアドレスから直接読む。写していないので、一致すれば「XIP をそのまま
  //     渡している」ことの確認になる。
  const char *contents = (const char *)first;
  for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index)
    if (contents[index] != PAYLOAD[index]) {
      ++failures;
      break;
    }
  KERNEL::BOARD::diag_printf(
      "[FLASHFS] read %lu bytes straight from %p -> %s\n",
      (unsigned long)PAYLOAD_BYTES, (void *)first,
      failures == 0 ? "matches" : "MISMATCH");
  return failures;
}

} // namespace objects
} // namespace shizuku
