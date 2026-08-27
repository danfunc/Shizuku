#ifndef SHIZUKU_STREAM_HPP
#define SHIZUKU_STREAM_HPP
#include <cstdint>

// ===========================================================================
//  ストリーム — オブジェクト間 / コア間のデータの流れ (DESIGN §13)
// ===========================================================================
//  【柱 1: 制御プレーンとデータプレーンを分ける】
//    - 制御 (create/open/bind/connect) は svc でカーネルオブジェクトへ委ねる。
//      登録・単一 producer / 単一 consumer の強制・接続はそこでしかできない
//    - データ (push/pop) は **svc を通らないライブラリ**。単一アドレス空間では
//      push/pop を svc 化しても保護は 1bit も増えず、1 レコードあたり 100 サイクル
//      以上の体裁代を払うだけになる
//
//  【柱 2: 何を運ぶかは用途で変える】
//    ★flash の読みを「バイトの流れ」にしてはいけない。あれは XIP で**アドレスを
//      そのまま返せる**のが取り柄 (D21) で、バイトに直した瞬間にその取り柄が消える。
//      だから読みのストリームは `extent` (アドレス + 長さ) を運ぶ。**写さない**。
//    ★逆に書きはバイトを運ぶ。締切のある側が flash の 33ms を待たされないための
//      緩衝がまさに要るところで、そこは写す価値がある (D34)。
//
//  【柱 3: 単一 producer / 単一 consumer (SPSC)】
//    wr / rd は**巻き戻さない**通し番号で、場所は剰余で出す。番号そのものが
//    「何個目か」を表すので、読み手は追い越されたかどうかを番号だけで判断できる。
//    2 コアで同時に触るので、公開は release、観測は acquire で行う。
namespace shizuku {
namespace stream {

// 溢れたときの振る舞い。★どちらが正しいかは用途で決まるので、機構は選ばない。
constexpr uint32_t LOSSLESS = 1u << 0; // 溢れたら押し戻す (取りこぼさない)
// ★★ここに MP_PROD (「producer が複数」) という旗が**あった**が、消した
//   (2026-08-24, D46)。理由は 2 つあり、どちらも致命的だった:
//   (a) **定義されているだけで push() が一度も見ていなかった**。旗を立てても
//       何も起きず、「対応しているつもり」で複数から push させると黙って
//       レコードが消える (無音の失敗そのもの)
//   (b) そもそも**モデルに無い**。ストリームは object 対 object の路で、
//       席 (producer/consumer) はオブジェクト単位に stream_bind が座らせる。
//       「producer が複数」は 1 本の路に複数のオブジェクトが繋がることになり、
//       前提と正面から矛盾する
//   → **複数から流したいなら、路をその数だけ作り、集約するなら**
//     **ハブオブジェクトを立てる**のが正しい形:
//         producer A ──stream──┐
//         producer B ──stream──┼─→ [hub] ──stream──→ consumer
//         producer C ──stream──┘
//     こうすればどのリンクも object 対 object のまま保たれ、しかも
//     「どう混ぜるか・どれを優先するか・溢れたらどれを捨てるか」という**方針が
//     ハブという 1 つのオブジェクトの中に居る** (方針をカーネルにもこの
//     ライブラリにも置かない = D1)。D37 が「詰め替えが要るなら中継オブジェクトを
//     書くべき」と言っているのと同じ話の、合流版。

// 制御プレーンの実体。カーネルオブジェクトはこのポインタだけを持つ。
// ★データ領域 (base) と分離して置く。将来 MPU でデータ側だけを保護できるように
//   するため (ディスクリプタは保護の内側、データは外側に置ける)。
struct descriptor {
  void *base;         // データ領域の先頭 = REC buffer[capacity]
  uint32_t rec_size;  // 1 レコードのバイト数
  uint32_t capacity;  // レコード数
  uint32_t flags;     // LOSSLESS
  volatile uint32_t wr; // 公開済みレコード数 (producer が進める)
  volatile uint32_t rd; // 消費済みレコード数 (consumer が進める)
  uint32_t producer;  // 席を取っているオブジェクト (NO_OWNER = 空き)
  uint32_t consumer;
};

constexpr uint32_t NO_OWNER = 0xFFFFFFFFu;
// ★接続が席に座っている印。実オブジェクトの番号と衝突しない値にしてある —
//   座っている限りオブジェクトは bind できない (接続と手押しの二重供給を防ぐ)。
constexpr uint32_t CONNECTED = 0xFFFFFFFEu;

enum struct role : uint32_t { PRODUCER = 0, CONSUMER = 1 };

// 追い越しを検出したときに、どれだけ戻って読み直すかの余裕。
constexpr uint32_t RESYNC_MARGIN = 2;

// ---- データプレーン (svc を通らない) ---------------------------------------
// ★always_inline にするのは、呼び出し元の配置を継承させるため。RAM 常駐の
//   producer から呼べば push も RAM に入り、XIP の取り合いを避けられる。
#define SHIZUKU_STREAM_INLINE [[gnu::always_inline]] inline

template <typename REC> class handle {
public:
  handle() = default;
  explicit handle(descriptor *target) : m_desc(target) {}
  bool valid() const { return m_desc != nullptr; }
  descriptor *desc() const { return m_desc; }

  // 1 つ流す。LOSSLESS で満杯なら false (押し戻す)。そうでなければ最も古いものを
  // 上書きして必ず true。★producer は決して待たない — 待つと、締切のある側が
  //   遅い側に引きずられる (それを避けるためにストリームがある)。
  //
  // ★★★**この路へ push してよいのは 1 つのオブジェクトだけ** (柱 3)。
  //   ストリームは object 対 object の路で、席は stream_bind がオブジェクト
  //   単位で座らせる。**複数から流したいなら路をその数だけ作り、集約するなら
  //   ハブオブジェクトを立てること** (上の LOSSLESS の脇の図を参照)。
  //   ここは「気をつける」で守るしかない場所になっている: push/pop は
  //   わざと svc を通らないライブラリなので (柱 1)、descriptor のポインタさえ
  //   持っていれば bind を通らずに呼べてしまい、カーネルオブジェクト側の
  //   席の強制 (SEAT_TAKEN) をすり抜けられる。
  //   ★破るとどうなるか (2026-08-24 に消費側で実際に起きた): 下の
  //   「wr を読む → slot へ書く → wr+1 を書き戻す」の途中で別の producer に
  //   割り込まれると、両者が同じ場所へ書いて wr が 1 しか進まない。
  //   **エラーも出ずにレコードが消える**。
  SHIZUKU_STREAM_INLINE bool push(const REC &record) {
    const uint32_t capacity = m_desc->capacity;
    const uint32_t wr = m_desc->wr;
    const uint32_t rd = __atomic_load_n(&m_desc->rd, __ATOMIC_ACQUIRE);
    if ((m_desc->flags & LOSSLESS) && (wr - rd) >= capacity)
      return false;
    slot(wr) = record;
    // ★中身を置いてから番号を進める。逆にすると、揃う前の場所を読ませてしまう。
    __atomic_store_n(&m_desc->wr, wr + 1, __ATOMIC_RELEASE);
    return true;
  }

  // 1 つ取り出す。空なら false。lost には「追い越されて落ちた数」が入る。
  // ★落ちた数を**必ず返す**。黙って詰めると「取りこぼしていないように見える」
  //   計測事故になる (DESIGN §16)。
  SHIZUKU_STREAM_INLINE bool pop(REC *into, uint32_t *lost = nullptr) {
    const uint32_t capacity = m_desc->capacity;
    const uint32_t wr = __atomic_load_n(&m_desc->wr, __ATOMIC_ACQUIRE);
    uint32_t rd = m_desc->rd;
    if (rd == wr)
      return false;
    uint32_t dropped = 0;
    if ((wr - rd) > capacity) {
      // 追い越された。読める一番古いところまで飛ばす。
      const uint32_t fresh = wr - capacity + RESYNC_MARGIN;
      dropped = fresh - rd;
      rd = fresh;
    }
    *into = slot(rd);
    // ★写してから、写した場所がまだ生きていたかを確かめる。写している最中に
    //   一周されていたら、その中身は別物なので落とす (D33 と同じ考え方)。
    const uint32_t after = __atomic_load_n(&m_desc->wr, __ATOMIC_ACQUIRE);
    if ((after - rd) > capacity) {
      const uint32_t fresh = after - capacity + RESYNC_MARGIN;
      dropped += fresh - rd;
      __atomic_store_n(&m_desc->rd, fresh, __ATOMIC_RELEASE);
      if (lost != nullptr)
        *lost = dropped;
      return false;
    }
    __atomic_store_n(&m_desc->rd, rd + 1, __ATOMIC_RELEASE);
    if (lost != nullptr)
      *lost = dropped;
    return true;
  }

  uint32_t available() const {
    const uint32_t wr = __atomic_load_n(&m_desc->wr, __ATOMIC_ACQUIRE);
    return wr - m_desc->rd;
  }

private:
  SHIZUKU_STREAM_INLINE REC &slot(uint32_t index) {
    return static_cast<REC *>(m_desc->base)[index % m_desc->capacity];
  }
  descriptor *m_desc = nullptr;
};

// 生成側が持つ実体。★記憶を持つのは生成したオブジェクトで、カーネルオブジェクトは
//   ディスクリプタを指すだけ (資源はオブジェクトが持つ = DESIGN §4.1 ルール 1)。
template <typename REC, uint32_t CAPACITY> struct storage {
  descriptor desc;
  REC buffer[CAPACITY];

  void init(uint32_t flags = 0) {
    desc.base = buffer;
    desc.rec_size = sizeof(REC);
    desc.capacity = CAPACITY;
    desc.flags = flags;
    desc.wr = 0;
    desc.rd = 0;
    desc.producer = NO_OWNER;
    desc.consumer = NO_OWNER;
  }
  handle<REC> hdl() { return handle<REC>(&desc); }
};

// flash の読みが運ぶもの。★バイトではなく**在り処**。写さないための型。
struct extent {
  uintptr_t address; // そのまま読める XIP アドレス
  uint32_t bytes;
  uint32_t sequence; // 何個目か (追い越しの検出と、順序の確認に使う)
};

} // namespace stream
} // namespace shizuku
#endif // SHIZUKU_STREAM_HPP
