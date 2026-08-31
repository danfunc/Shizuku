#ifndef SHIZUKU_TEMPLATES_KERNEL_HPP
#define SHIZUKU_TEMPLATES_KERNEL_HPP
#include <cstdint>
#include "shizuku/kernel_abi.hpp"
#include "shizuku/templates/thread.hpp"

namespace shizuku {
namespace templates {

// ===========================================================================
//  カーネル — 機構だけを持つ層 (docs/03_porting_policy.md D1 / DESIGN §7)
// ===========================================================================
//  カーネルが知ること: スレッド / 文脈 / スタック上限 / 呼び出しフレームの push・pop /
//    実行権
//  カーネルが知らないこと: オブジェクト、オブジェクト ID、identity、メソッド表、
//    export、md、svc 番号の意味、そして「誰が偉いか」
//
//  ★★2 つの「svc ハンドラ」は別概念。混ぜないこと:
//    (1) **カーネルの svc ハンドラ** = svc_dispatch。例外文脈 (Handler モード) で
//        走る機構。経路を決め、プリミティブを実行する。番号の意味は持たない
//    (2) **オブジェクトランドの svc ハンドラ** = カーネルオブジェクトが持ち、
//        **スレッドモードで**走る方針側。番号を解釈し、担当へ配る
//
//  ★経路は**呼び出しフレームの段数のパリティ**だけで決まる (I-1):
//      偶数段 → オブジェクトが走っている → (2) をメソッドとして呼ぶ
//      奇数段 → ハンドラが走っている     → プリミティブを実行
//    積むのはトランポリンと CALL の 2 つだけで必ず交互になるので、パリティが
//    そのまま実行主体を表す。旗も cookie も要らない。段数を書けるのはカーネル
//    だけなので偽装もできない (オブジェクトが誰かは kobj の台帳の話。PORT §3.1)。
//
//  ★カーネルオブジェクト以外は RETURN を撃てない。そのため、ハンドラを起こすときに
//    **今のネスト数を渡し** (ARCH::set_handler_info)、オブジェクトは exit API に
//    **何段戻すか**を載せて撃ち、ハンドラがその段数で巻き戻す (D5)。
template <typename CPU_MANAGER_T, typename MEMORY_MANAGER_T>
class kernel {
public:
  using CPU_MANAGER = CPU_MANAGER_T;
  using MEMORY_MANAGER = MEMORY_MANAGER_T;
  using ARCH = typename CPU_MANAGER::ARCH;
  using BOARD = typename CPU_MANAGER::BOARD;
  using CONTEXT = typename ARCH::context_t;
  using FRAME = typename ARCH::exception_frame_t;
  using THREAD = thread<CONTEXT>;
  static constexpr uintptr_t CORE_COUNT = CPU_MANAGER::CORE_COUNT;
  static constexpr uint32_t KERNEL_OBJECT_ID = 0;

  // ★スレッドの記憶はカーネルの持ち物ではない。**オブジェクトランドが用意して貸す**
  //   (DESIGN §4.1 ルール 1「オブジェクトが資源を持つ」)。カーネルは渡された記憶を
  //   使うだけで、いくつ作れるかも渡された量が決める。
  //   ★ただし置き場所には条件がある: カーネルの簿記は**非特権から到達できない場所**
  //     でなければ、非特権オブジェクトが他スレッドの文脈を書き換えられてしまう。
  //     所有 (誰が用意するか) と保護 (どこに置くか) は別の話なので、前者は
  //     オブジェクトランドに渡し、後者はここで**検査する** (気をつけるでは守れない)。
  struct thread_record {
    THREAD thread;
    CONTEXT context;
  };
  static constexpr uintptr_t thread_record_bytes() {
    return sizeof(thread_record);
  }
  void set_thread_storage(void *memory, uintptr_t bytes);
  uint32_t thread_count() const { return m_thread_count; }

  // -------------------------------------------------------------------------
  //  呼び出しフレーム — 退避先は**スレッド自身のスタック** (DESIGN §8)
  // -------------------------------------------------------------------------
  //  【幾何】スタックは下へ伸びる。呼び出し元の生スタック境界を X とすると:
  //
  //    低位 ←                                                        → 高位
  //    [ 呼び先が使う ][ 書き換え用フレーム ][ ヘッダ + 元の例外フレーム ][ 呼び出し元 ]
  //                    X-total-frame       X-total                    X
  //
  //  ★I-3: 元の例外フレームは 1 バイトも動かさない。退避域の一番上に元の位置のまま
  //    置き、下へ複製するのは書き換え用の作業コピーだけ (FP 拡張フレームの
  //    S0-S15 と乖離させないため)。
  //  ★I-4: 呼び先が復帰した直後の SP が退避域へ食い込んではならない。ISA が復帰時に
  //    SP を追加調整する場合 (ARMv8-M の xPSR bit9 による +4 など) は
  //    ARCH::normalize_frame で作業コピー側の調整を消してから積む。push はその結果を
  //    ARCH::psp_after_return で必ず検算する。
  //  ★I-5: pop は push 時に記録した値を読み戻す。再計算しない。
  struct call_frame_header {
    uintptr_t prev;       // 一つ外側のヘッダのアドレス (0 = 最外)
    uint32_t total_bytes; // 退避域の総バイト数 (8B 境界に丸め済み)
    uint32_t frame_bytes; // 例外フレーム実サイズ
    uint32_t caller_object; // 呼び出し元オブジェクトID
    CONTEXT saved; // 呼び出し元の文脈まるごと (sp を含む = 元フレームの位置)
  };

  CPU_MANAGER cpu_manager;
  MEMORY_MANAGER memory_manager;

  // 自コアの例外結線とメモリマネージャを初期化する。
  void init();
  // オブジェクトランドの svc ハンドラを据える。系の組み立て (composition) の一部で
  // 実行時 API ではないため、ブート前に 1 回だけ呼ぶ。
  void set_object_handler(uintptr_t entry_pc);
  // 今の実行をスレッド 0 として採用し、entry へ移る (スレッドスタックへ
  // 切り替えるので戻らない)。★最初の 1 本のスタックも貸してもらう — ここだけ
  //   カーネルが自分で malloc すると「スレッドの記憶は誰のものか」が二枚舌になる。
  [[noreturn]] void bootstrap(void (*entry)(), uintptr_t stack_base,
                              uintptr_t stack_bytes);
  // 2 本目以降のコアが自分で呼ぶ。今の実行を **指定されたスレッド** として採用し、
  // entry へ移る。★スレッド 0 を使えないので枠を指定する形になる — 誰をそのコアの
  // 最初の 1 本にするかは方針なので、決めるのはオブジェクトランド。
  [[noreturn]] void bootstrap_secondary(uint32_t thread, void (*entry)(),
                                        uintptr_t stack_base,
                                        uintptr_t stack_bytes);

  // -------------------------------------------------------------------------
  //  スレッドの生成 — カーネルオブジェクトが**スレッドモードから**呼ぶ C++ API
  // -------------------------------------------------------------------------
  //  ★syscall にしない。スタックを確保する (= malloc する) 操作なので例外文脈で
  //    やってはいけない。フレームも触らないので syscall である必要がない。
  //  誰のために作るか・何を走らせるかは方針なので、決めるのは呼ぶ側 (kobj)。
  //  entry が return するとスレッドは終わる (戻り先が無いので kobj が終了させる)。
  struct spawn_result {
    kernel_error error;
    uint32_t thread;
  };
  // ★スタックも**呼ぶ側が用意して渡す**。どれだけの深さを許すかは方針であって、
  //   カーネルが決めることではない (カーネルは溢れを検出して止めるだけ)。
  struct spawn_request {
    uintptr_t entry_pc;
    uintptr_t argument;
    uint32_t protection;
    uint32_t affinity;
    uintptr_t stack_base;
    uintptr_t stack_bytes;
    uint32_t object_id = 0; // 所属オブジェクトID
    // ★call_request と同じ窓 (軸 B)。この対象オブジェクトの**自前のスレッド**
    //   としての初回起動にも、CALL のときと同じ窓を開ける (Q8)。
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
  };
  spawn_result spawn(const spawn_request &request);
  // 走らせずに枠だけ取る。2 本目以降のコアが「今の実行」を採用するために使う
  // (spawn は入口から走らせる形なので、採用には使えない)。
  spawn_result reserve_thread();
  // 終わったスレッドの枠を返す。記憶を返すのは貸し主 (オブジェクトランド) の仕事。
  void release(uint32_t thread);
  // スレッドを終了させる (走り終えた / 隔離する)。今のコアが走らせているスレッドを
  // 終了させた場合は、次に誰かへ切り替わるまでこのコアは何もしない。
  void terminate(uint32_t thread);
  // ★**READY からのみ**終了させる (取れたら true)。「他人を終わらせて記憶を
  //   返してよいか」を一手で決めるための口 (D55)。READY はこの系では
  //   「文脈の退避が済み、どのコアも走らせていない」を意味するので、
  //   これが CAS で取れた瞬間だけが足場を返してよい瞬間になる。
  //   ★RUNNING / WAIT_GRANT からは取らない: 前者は他コアが今その足場で
  //     走っており、後者は貸し手として巻き取りを待っている。
  //   ★SUSPENDED からも取らない。あれは**デバッガの持ち物**なので、
  //     こちらが横から終わらせるとデバッガが止めた相手が消える。
  //     デバッガが再開して READY へ戻したところで、こちらの番が来る。
  bool terminate_if_idle(uint32_t thread);

  // ---- ISA 層 (例外入口) から呼ばれる ----
  CONTEXT *current_context();
  void svc_dispatch(CONTEXT *context);
  void pendsv_dispatch(CONTEXT *context);
  // 保護違反やスタック上限違反で落ちたときの受け口。
  // ★**系を止めない**。落ちたのは 1 つのスレッドなので、そのスレッドだけを止めて
  //   他は走り続けさせる (I-9 / DESIGN §11.2.4)。止めても直らないのは
  //   「カーネル自身が落ちた」場合だけで、そこは区別する。
  void fault_dispatch(CONTEXT *context);
  // ---- 自己ホスト型デバッグの受け口 ---------------------------------------
  //  ★カーネルがやるのは**機構だけ**: 「何が起きたか」を記録し、そのスレッドを
  //    止め、他へ実行権を渡す。何を報告するか・いつ再開するかは方針なので、
  //    デバッガのオブジェクトが決める (D1)。カーネルは GDB を知らない。
  void debug_dispatch(CONTEXT *context);
  // そのスレッドだけ止める / 戻す。★TERMINATED と違って**文脈は生きたまま**
  //   なので、レジスタを見て書き換えてから再開できる。
  void suspend(uint32_t thread);
  void resume(uint32_t thread);
  // ★★1 命令だけ実行させたい相手を**予約する**。ここで直接 MON_STEP を立てては
  //   いけない — 立てた瞬間に「次の 1 命令」が刻まれるのは**呼び出し側 (デバッガの
  //   スレッド) の次の命令**であって、対象が次に走ったときの命令ではない
  //   (実測で踏んだ: デバッガ自身が single-step で止まり、GDB チャネルごと
  //   死んだ)。実際に MON_STEP を立てるのは、対象の文脈へ復帰する
  //   CTX_RESTORE の直前 (= consume_pending_step) だけ。
  void arm_step(uint32_t thread);
  // 復帰しようとしている文脈がちょうど予約した相手なら、ここで初めて
  // MON_STEP を立てる。ISA 層の例外復帰 (SVC/PendSV/DebugMon/Fault の
  // どれでも) から毎回呼ばれるので、対象でなければ何もしない。
  void consume_pending_step();
  static constexpr uint32_t NO_STEP_TARGET = 0xFFFFFFFFu;
  // 止まっているスレッドの文脈。デバッガがレジスタを読み書きするための口。
  CONTEXT *thread_context(uint32_t thread) {
    return thread < m_thread_count ? &m_threads[thread].context : nullptr;
  }
  // スレッドが落ちたときに実行権を渡す先。誰に渡すかは方針なので、composition の
  // 段階でカーネルオブジェクトが教えておく (カーネルは選ばない)。
  void set_recovery_thread(uint32_t thread);

  bool thread_debug_protected(uint32_t thread) const {
    return thread < m_thread_count && m_threads[thread].thread.is_debug_protected;
  }
  void set_thread_debug_protected(uint32_t thread, bool is_protected) {
    if (thread < m_thread_count)
      m_threads[thread].thread.is_debug_protected = is_protected;
  }

  uint32_t current_thread_id() const { return m_current[BOARD::core_num()]; }
  THREAD &current_thread() { return m_threads[current_thread_id()].thread; }
  const THREAD &current_thread() const {
    return m_threads[current_thread_id()].thread;
  }
  uint32_t current_depth() const { return current_thread().call_stack.depth; }
  // スケジューリング方針 (kobj 側) が候補を探すための読み出し。
  // そのスレッドを走らせてよいコアの集合 (方針側が候補を絞るために読む)。
  uint32_t thread_affinity(uint32_t thread) const {
    return m_threads[thread].thread.affinity;
  }
  void set_thread_object(uint32_t thread, uint32_t obj) {
    m_threads[thread].thread.current_object = obj;
  }
  // その枠が今何代目か。★スレッド番号を控える側は、これも一緒に控えて
  //   使う直前に突き合わせること (番号だけでは使い回しに気づけない)。
  uint32_t thread_generation(uint32_t thread) const {
    return thread < m_thread_count ? m_threads[thread].thread.generation : 0;
  }
  typename THREAD::state_t thread_state(uint32_t thread) const {
    return (typename THREAD::state_t)m_threads[thread].thread.state;
  }
  // 今このコアで実行権を借りて走っているか (借り手の yield は貸し手への早期復帰)。
  bool grant_active() const { return m_grants[BOARD::core_num()].depth != 0; }

  // タイマ例外から呼ばれる (期限の監視。切替そのものは最低優先度の遅延例外で行う)。
  void timer_expired();

private:
  bool call_frame_push(THREAD &thread, CONTEXT *context, FRAME **frame);
  bool call_frame_pop(THREAD &thread, CONTEXT *context, FRAME **frame);

  kernel_error do_call(THREAD &thread, CONTEXT *context, FRAME **frame,
                       const call_request &request);

  kernel_error do_switch(uint32_t target);
  kernel_error do_grant(uint32_t target, uint32_t cycles);
  // 貸した実行権を 1 段巻き取って貸し手へ戻す (期限切れ / 早期復帰の共通経路)。
  void grant_unwind(grant_end reason);
  // 今の刻みで使ったぶんを全段から引く。**貸し借りに触る前に必ず呼ぶ**。
  void grant_charge();
  // 一番内側の残りをタイマへ装填する (幅が足りなければ刻んで継ぎ足す)。
  void arm_timer();
  bool claim(uint32_t thread, kernel_error &error);

  // 実行権の貸し出しスタック (per-core)。ネストできるが、内側の残量は外側の残量で
  // クランプされるので借りた以上は又貸しできない (I-7)。
  // ★単位は**クロック** (µs ではない)。理由:
  //   (1) SysTick が数えているのはクロックなので、µs で持つと装填のたびに
  //       clk_sys で割り戻すことになる。**その clk_sys が変わらない保証がない**
  //       (オーバークロック、将来の周波数切替)。貸している最中に変われば、
  //       換算済みの期限は静かにずれ、予定どおりに返ってこない
  //   (2) 貸し手が本当に縛りたいのは「どれだけ**仕事**をしてよいか」で、仕事は
  //       おおよそクロック数。µs で書くとクロックを上げた瞬間に、同じ数字が
  //       黙って倍の仕事を意味するようになる。クロックで書けば意味が動かない
  //   ★対して SLEEP は µs のまま。あちらは壁時計の話 (「20ms 後に起こして」) で、
  //     仕事量ではない。**別の量なので単位を揃えてはいけない**。
  struct grant_frame {
    uint32_t lender;    // 貸し手 (WAIT_GRANT で待っている)
    uint64_t remaining; // 残りクロック数 (外側でクランプ済み)
  };
  struct grant_stack {
    static constexpr uint32_t MAX_DEPTH = 8;
    grant_frame frames[MAX_DEPTH];
    uint32_t depth;
  };

  // 貸してもらった記憶。カーネルはここを所有しない。
  thread_record *m_threads;
  uint32_t m_thread_count;
  uint32_t m_current[CORE_COUNT];
  // ★予約は 1 件だけ (デバッガは 1 セッションしか繋がらない)。CORE_COUNT では
  //   なくコア非依存で持つ — どのコアが対象を復帰させるかは分からない
  //   (対象スレッドはどのコアでも走れるのが既定)。
  uint32_t m_step_target;
  grant_stack m_grants[CORE_COUNT];
  // 今タイマへ装填した刻みの大きさ [クロック]。残りから引くために覚えておく。
  uint32_t m_armed[CORE_COUNT];
  // ★取り上げを見送ったときに、次に見に来るまでの間隔 [クロック]。
  //   借り手がオブジェクトランドのハンドラの中に居る間は切り替えてはいけない
  //   (下の pendsv_dispatch を参照)。短すぎると見送りの割り込みだけが増え、
  //   長すぎると取り上げが遅れる。参照実装は 50µs 相当を使っていた。
  static constexpr uint32_t GRANT_RETRY_CYCLES = 8192; // ≒55µs @150MHz
  // オブジェクトランドの svc ハンドラの入口。表ではなく 1 個だけ。
  uintptr_t m_object_svc_handler;
  uint32_t m_recovery_thread;

public:
  // 落ちたスレッドの記録 (自己テストと診断が読む)。
  struct fault_record {
    uint32_t count;    // 何本止めたか
    uintptr_t pc;      // 最後に落ちた場所
    uint32_t status;   // 違反の種類 (CFSR)
    uint32_t thread;   // 止めたスレッド
  };
  const fault_record &faults() const { return m_faults; }

  // 直近のデバッグ事象。★フォールトと別に持つ — 「落ちた」と「止めた」は
  //   別のことで、混ぜると数えられなくなる。
  // ★止めようとしたが渡す先が無くて続けさせた、という印。reason に足して返す
  //   (DFSR の実ビットとぶつからない上位を使う)。
  static constexpr uint32_t DEBUG_NOT_STOPPED = 1u << 31;
  struct debug_record {
    uint32_t count;  // 何回止まったか
    uint32_t thread; // 止めたスレッド
    uintptr_t pc;    // 止まった場所
    uint32_t reason; // DFSR (BKPT / 1 命令実行 / ウォッチポイント)
  };
  const debug_record &debug_event() const { return m_debug; }
  // ★回数だけは**必ず読み直す**口を別に用意する。m_debug を書くのは例外ハンドラ
  //   なので、コンパイラから見ると「誰も書いていない」= 素直に読むとレジスタに
  //   載ったまま更新されない。実際にそれで「事象が起きているのに回数が増えて
  //   いない」という嘘の失敗を出した (間に関数呼び出しがある経路だけ、たまたま
  //   正しく見えていた)。
  uint32_t debug_event_count() const {
    return ARCH::load_acquire32((volatile uint32_t *)&m_debug.count);
  }

private:
  fault_record m_faults;
  debug_record m_debug;
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_HPP
