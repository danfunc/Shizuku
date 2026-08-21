// ===========================================================================
//  flash FS オブジェクト — XIP 前提の、アドレスを返すファイル系 (実装)
// ===========================================================================
//  設計の理由は shizuku/objects/flash_fs.hpp を参照。ここは機械の都合を書く。
#include "hardware/flash.h"
#include "hardware/sync.h"
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
// ★多コアでは割り込みを落とすだけでは足りない。もう一方のコアが消去中に flash を
//   踏むと即死するので、相手を RAM 上のコードへ退避させて止める必要がある
//   (pico-sdk の flash_safe_execute)。**まだ入れていない**ので、多コア構成では
//   書き込みを断る (黙って壊れるより断る)。読みは XIP なので多コアでも安全。
constexpr bool FLASH_WRITE_IS_SAFE = (KERNEL::CORE_COUNT == 1);

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
constexpr uint32_t DIRECTORY_VERSION = 1;

// 目録は 1 セクタに収める。可変長にすると目録が育つたびに引っ越しが要るが、
// 引っ越しはアドレスを動かすことなので、この FS では**やってはいけない**操作。
struct directory_header {
  uint32_t magic;
  uint32_t version;
  uint32_t bump;    // 次に置ける XIP アドレス (セクタ境界)
  uint32_t entries; // 居るファイル数 (走査を早く終わらせるためだけの数)
};
struct directory_entry {
  char name[FLASH_NAME_BYTES];
  uint32_t address; // XIP アドレスそのもの。**引いた結果がこれ**
  uint32_t bytes;   // 実バイト数 (セクタ境界まで丸めた分は含めない)
};
constexpr uint32_t ENTRY_COUNT =
    (FLASH_SECTOR_SIZE - sizeof(directory_header)) / sizeof(directory_entry);

// 目録の作業コピー。**RAM に無いといけない**理由が 2 つある:
//   (1) 書き込み中は XIP が止まるので、書き込み元が flash 上にあると読めない
//   (2) 消去は目録セクタごと消すので、消す前に中身を持っていないと復元できない
struct directory_image {
  directory_header header;
  directory_entry entries[ENTRY_COUNT];
};
static_assert(sizeof(directory_image) <= FLASH_SECTOR_SIZE, "目録は 1 セクタ");
directory_image g_directory;
bool g_mounted = false;

// 書き込み元を 1 ページずつ RAM へ写すための中継。**XIP が止まっている間に
// 呼び出し側のバッファ (文字列リテラルなら flash 上にある) を読むことはできない**
// ので、写してから渡す。
alignas(4) uint8_t g_page[FLASH_PAGE_SIZE];

constexpr uint32_t sector_round_up(uint32_t bytes) {
  return (bytes + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
}

bool name_equal(const char *left, const char *right) {
  for (uintptr_t index = 0; index < FLASH_NAME_BYTES; ++index) {
    if (left[index] != right[index])
      return false;
    if (left[index] == '\0')
      return true;
  }
  return true; // 24 文字ぴったりまで一致 (終端無しで焼かれている)
}

void name_copy(char *destination, const char *source) {
  uintptr_t index = 0;
  for (; index + 1 < FLASH_NAME_BYTES && source[index] != '\0'; ++index)
    destination[index] = source[index];
  for (; index < FLASH_NAME_BYTES; ++index)
    destination[index] = '\0';
}

int find_entry(const char *name) {
  for (uint32_t index = 0; index < ENTRY_COUNT; ++index) {
    if (g_directory.entries[index].address == 0)
      continue;
    if (name_equal(g_directory.entries[index].name, name))
      return (int)index;
  }
  return -1;
}

void directory_reset() {
  g_directory.header.magic = DIRECTORY_MAGIC;
  g_directory.header.version = DIRECTORY_VERSION;
  // 目録が先頭 1 セクタを使うので、置き場所はその次から。
  g_directory.header.bump = (uint32_t)(REGION_ADDRESS + FLASH_SECTOR_SIZE);
  g_directory.header.entries = 0;
  for (uint32_t index = 0; index < ENTRY_COUNT; ++index) {
    g_directory.entries[index].address = 0;
    g_directory.entries[index].bytes = 0;
    g_directory.entries[index].name[0] = '\0';
  }
}

// ---- 媒体へ書く ------------------------------------------------------------
// ★ここが「系を止める」区間。割り込みを落としてから消去・書き込みを行い、必ず
//   戻す。pico-sdk の flash_range_* は RAM 上で走る関数として置かれており、
//   終わりに XIP のキャッシュも捨ててくれる (捨てないと、書いた直後に読んだとき
//   古い中身が見える)。
// ★将来オーバークロックを入れるときの注意 (docs/03 D23): 消去・書き込みは XIP を
//   一度落として入り直す経路なので、**QMI のタイミング設定が ROM 既定へ戻る**恐れが
//   ある。今は SDK 既定の 150MHz で ROM 既定のまま走っているので実害が無いだけで、
//   フラッシュを速める設定を入れた瞬間にここが「書いた後だけ読み違える」経路になる。
//   速すぎる QMI 設定は落ちずに**静かにデータを読み違える**ので、気づけない。
void flash_write(uint32_t offset, const uint8_t *ram_data, uint32_t bytes,
                 bool erase_first, uint32_t erase_bytes) {
  const uint32_t interrupts = save_and_disable_interrupts();
  if (erase_first)
    ::flash_range_erase(offset, erase_bytes);
  if (bytes != 0)
    ::flash_range_program(offset, ram_data, bytes);
  restore_interrupts(interrupts);
}

// 目録を焼き直す。作業コピーは RAM にあるのでそのまま渡せる。
void directory_flush() {
  flash_write(REGION_OFFSET, (const uint8_t *)&g_directory,
              FLASH_SECTOR_SIZE, true, FLASH_SECTOR_SIZE);
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
  request->address = g_directory.entries[slot].address;
  request->bytes = g_directory.entries[slot].bytes;
  return request->address;
}

uintptr_t flash_store_method(uintptr_t argument, uintptr_t, uintptr_t,
                             uintptr_t) {
  flash_store *request = (flash_store *)argument;
  if (request == nullptr || request->name == nullptr || !g_mounted)
    return 0;
  if (request->data == nullptr || request->bytes == 0)
    return 0;
  if (!FLASH_WRITE_IS_SAFE)
    return 0; // 多コアではもう一方を止める算段が要る (上の注記)

  const uint32_t reserved = sector_round_up(request->bytes);
  const uint32_t bump = g_directory.header.bump;
  if (bump + reserved > REGION_ADDRESS + REGION_BYTES)
    return 0; // 空きが無い。詰め直しはしない (アドレスが動くので)

  int slot = find_entry(request->name);
  if (slot < 0) {
    for (uint32_t index = 0; index < ENTRY_COUNT; ++index) {
      if (g_directory.entries[index].address == 0) {
        slot = (int)index;
        break;
      }
    }
    if (slot < 0)
      return 0; // 目録が満杯
    ++g_directory.header.entries;
  }
  // 同じ名前が既に居た場合、古い領域は**捨てる**。上書きしないのは、上書きには
  // 消去が要り、消去の単位はセクタなので、大きさが変われば結局引っ越すため。

  const uint32_t offset = bump - XIP_BASE;
  // まず置き場所を空ける (セクタ単位)。
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

  g_directory.entries[slot].address = bump;
  g_directory.entries[slot].bytes = request->bytes;
  name_copy(g_directory.entries[slot].name, request->name);
  g_directory.header.bump = bump + reserved;
  directory_flush();

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
  // ★空くのは**名前だけ**。領域は返らない。返すには後ろを詰めるしかなく、
  //   詰めればアドレスが動く — アドレスこそがこの FS の API なので、動かせない。
  g_directory.entries[slot].address = 0;
  g_directory.entries[slot].bytes = 0;
  g_directory.entries[slot].name[0] = '\0';
  if (g_directory.header.entries != 0)
    --g_directory.header.entries;
  directory_flush();
  return 1;
}

uintptr_t flash_list_method(uintptr_t argument, uintptr_t, uintptr_t,
                            uintptr_t) {
  flash_entry *request = (flash_entry *)argument;
  if (request == nullptr || !g_mounted || request->index >= ENTRY_COUNT)
    return 0;
  uint32_t seen = 0;
  for (uint32_t index = 0; index < ENTRY_COUNT; ++index) {
    if (g_directory.entries[index].address == 0)
      continue;
    if (seen++ != request->index)
      continue;
    name_copy(request->name, g_directory.entries[index].name);
    request->address = g_directory.entries[index].address;
    request->bytes = g_directory.entries[index].bytes;
    return 1;
  }
  return 0;
}

uintptr_t flash_format_method(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  directory_reset();
  directory_flush();
  g_mounted = true;
  return 1;
}

uintptr_t flash_status_method(uintptr_t argument, uintptr_t, uintptr_t,
                              uintptr_t) {
  flash_status *request = (flash_status *)argument;
  const uint32_t used = g_directory.header.bump - (uint32_t)REGION_ADDRESS;
  if (request != nullptr) {
    request->region_address = REGION_ADDRESS;
    request->region_bytes = REGION_BYTES;
    request->used_bytes = used;
    request->free_bytes = REGION_BYTES - used;
    request->entries = g_directory.header.entries;
  }
  return used;
}

// ---- mount ----------------------------------------------------------------
// main は「自分の媒体を自分で立ち上げる」。目録を XIP からそのまま読み、名乗りが
// 合わなければ初期化する。**空の flash は 0xFF なので、初期化済みかどうかは
// magic で判る** (0 埋めを期待すると、消去直後の媒体を誤って読む)。
uintptr_t flash_fs_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const directory_image *media = (const directory_image *)REGION_ADDRESS;
  if (media->header.magic == DIRECTORY_MAGIC &&
      media->header.version == DIRECTORY_VERSION) {
    g_directory = *media;
    // ★焼かれているのは**絶対アドレス**なので、他所で作った像を読むと外を指す
    //   ポインタを配ってしまう。範囲に収まっているかをここで確かめ、収まって
    //   いなければ知らない媒体として扱う (黙って配らない)。
    bool sane = g_directory.header.bump >= REGION_ADDRESS + FLASH_SECTOR_SIZE &&
                g_directory.header.bump <= REGION_ADDRESS + REGION_BYTES;
    for (uint32_t index = 0; sane && index < ENTRY_COUNT; ++index) {
      const directory_entry &entry = g_directory.entries[index];
      if (entry.address == 0)
        continue;
      if (entry.address < REGION_ADDRESS + FLASH_SECTOR_SIZE ||
          entry.address + entry.bytes > REGION_ADDRESS + REGION_BYTES)
        sane = false;
    }
    if (!sane)
      directory_reset();
  } else {
    directory_reset(); // まだ何も焼かれていない (あるいは別物)
  }
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
uint32_t flash_fs_probe() {
  static const char PAYLOAD[] = "shizuku: an object is a thing that runs.";
  constexpr uint32_t PAYLOAD_BYTES = sizeof(PAYLOAD);
  uint32_t failures = 0;

  flash_lookup lookup{"probe.txt", 0, 0};
  api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
      (uintptr_t)flash_fs_method::LOOKUP, (uintptr_t)&lookup);
  const bool was_there = lookup.address != 0 && lookup.bytes == PAYLOAD_BYTES;

  if (!was_there) {
    // ★書き込み元は flash 上の文字列リテラル。**わざとそうしている** —
    //   XIP が止まっている間に読めないバッファを渡された場合でも壊れないことを、
    //   ここで実際に確かめておく (中継ページを通しているので通るはず)。
    flash_store store{"probe.txt", PAYLOAD, PAYLOAD_BYTES, 0};
    const uint64_t began = KERNEL::BOARD::time_us();
    api(object_api::CALL_METHOD, FLASH_FS_OBJECT,
        (uintptr_t)flash_fs_method::STORE, (uintptr_t)&store);
    const uint64_t took = KERNEL::BOARD::time_us() - began;
    if (store.address == 0) {
      KERNEL::BOARD::diag_printf("[FLASHFS] store failed\n");
      return 1;
    }
    // ★「系が止まる時間」を数字で残す。周期スレッドの隣で呼べる操作かどうかは、
    //   この数字を見てから決める話 (揺らぎを測ったときと同じ作法)。
    KERNEL::BOARD::diag_printf(
        "[FLASHFS] stored probe.txt at %p (%lu bytes), the system was stopped "
        "for %lu us\n",
        (void *)store.address, (unsigned long)PAYLOAD_BYTES,
        (unsigned long)took);
    lookup.address = store.address;
    lookup.bytes = PAYLOAD_BYTES;
  }

  // (1) 引いたアドレスから直接読む。写していないので、これが一致すれば
  //     「XIP をそのまま渡している」ことの確認になる。
  const char *contents = (const char *)lookup.address;
  for (uint32_t index = 0; index < PAYLOAD_BYTES; ++index) {
    if (contents[index] != PAYLOAD[index]) {
      ++failures;
      break;
    }
  }
  KERNEL::BOARD::diag_printf(
      "[FLASHFS] probe.txt %s: read %lu bytes straight from %p -> %s\n",
      was_there ? "survived the power cycle" : "written this boot",
      (unsigned long)lookup.bytes, (void *)lookup.address,
      failures == 0 ? "matches" : "MISMATCH");
  return failures;
}

} // namespace objects
} // namespace shizuku
