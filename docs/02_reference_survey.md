# 02. 参照実装 (latest_ver_from_flight_robocon) 調査結果

対象: `latest_ver_from_flight_robocon/flight_robocon_telemetory_sender/`。
RP2350 (pico2_w / Cortex-M33 デュアルコア) で実機運用されていた Shizuku + BLE テレメトリ。
**機能・設計の規範はここ** (ユーザー指示)。ただし「規範」の内訳は 2 層ある:

- **設計文書 (docs/) = 目標設計**。とくに `SHIZUKU_DESIGN.md` は「新リポジトリで
  これを読んで実装できること」を目標に書かれた仕様書で、**現行参照コードが持つ欠陥
  (§12) を直した姿**を規定している。
- **参照コード = 実機検証済みの機構カタログ**。設計文書が [実装済] とマークする部分の
  唯一の実装。ただし §12 の欠陥 (identity 潰し / 親子関係なし / md 未完) も内包する。
  → **コードを丸写ししない。設計文書を正として、機構の詳細だけコードから採る。**

## 1. 規範文書

| 文書 | 内容 | 重要度 |
|---|---|---|
| `docs/SHIZUKU_DESIGN.md` | 目標設計そのもの。設計原則 P1-P3、実行モデル、カーネル ABI (§7)、フレーム幾何 (§8)、巻き戻し (§9)、資源委譲 (§10)、保護 3 軸 (§11)、原設計との差分 (§12)、同期規約 (§14.5)、不変条件 I-1..I-9 (§15)、自己テスト (§16)、実装順序 (§17) | **必読・規範** |
| `docs/SHIZUKU_REDESIGN_PORTABILITY.md` | 移植戦略。ISA/ボード依存の棚卸し (§2)、カーネルからオブジェクト概念を外す (§3)、資源統一形 (§4)、巻き戻し規律 (§4.5)、委譲ファミリ (§4.6)、層構造 + concepts (§5)、移植順序 (§6)、検証インフラ (§7)、未決事項 (§8) | **必読・規範** |
| `HANDOFF.md` | セッション引き継ぎ。実機検証の履歴 (何が PASS/FAIL か)、ビルド手順、MPU ロードマップ (§6)、ハマりどころ | 実装時の参照 |
| `docs/XIP_FLASH_CLOCK.md` | クロック変更と固定分周の連鎖の実測 (board 層 API 化の根拠) | board 層設計時 |
| `docs/sensor_stream_protocol.md` | アプリ層プロトコル | 当面不要 |

## 2. 参照コードの地図 (カーネル関連のみ)

| ファイル | 行数 | 内容 |
|---|---|---|
| `kernel.cpp` | ~1200 | SVC/PendSV/SysTick ハンドラ本体、**call_frame_push/pop (フレーム幾何)**、grant スタック、try_claim (CAS)、MPU W^X、例外優先度、create_thread/async_call、panic ring、svc_trace |
| `include/kernel.hpp` | ~660 | context_t (**asm とオフセット共有**)、call_frame_hdr_t (幾何のコメントが最重要)、thread_t / object_t、svc 番号 enum、型付きエラー enum、ビルドトグル群 |
| `svc_asm_handler.S` | 129 | CTX_SAVE/CTX_RESTORE (r4-r11/PSP/EXC_RETURN/遅延FP S16-S31/PSPLIM/CONTROL RMW)。SVC と PendSV の入口 |
| `kernel_object.cpp` | ~780 | カーネルオブジェクト: kernel_obj_svc_handler (obj_api の巨大 switch)、sched_pick_next (budget round-robin)、scheduler_idle_loop、svc ルート表 (委譲)、md_table/obj_mem 固定表、ストリーム制御プレーン + DMA ポンプ |
| `include/obj_api.hpp` | 236 | 一般オブジェクト向け API 番号 enum + svc/svci ラッパ + ヘルパ (yield/sleep_us/run_for/async_call/set_affinity 等) |
| `include/svc.hpp` | 87 | svc<N> ラッパ、型付きエラー error_t<E> / svc_result_t |
| `include/svc_handler.hpp` | 44 | svc_handler_shim<F> (naked ABI シム。r4-r7 をスタック渡し引数化) |
| `include/shizu.hpp` | 53 | init() / thread0 ブートストラップ (set_current_context_as_kernel_init) |
| `include/stream.hpp` | 347 | SPSC ストリーム (データプレーンは svc を通らない) |
| `include/call_method.hpp` / `export_method.hpp` / `redirect_method.hpp` / `get_current_obj_id.hpp` | 小 | オブジェクト向けの薄いラッパ |
| `main.cpp` / `core1_boot.cpp` | | ブート、HardFault ダンプ、ビーコン、core1 起動 |
| `*_selftest.cpp` / `smp_stress.cpp` / `rt_sched_test.cpp` / `unpriv_probe.cpp` / `svc_delegate_test.cpp` | | **自己テスト群。DESIGN §16 の実体。新リポジトリでも同型を最初から用意する** |

## 3. 実機検証済みの機構 (移植価値が高い順)

1. **呼び出しフレーム幾何 (kernel.hpp `call_frame_hdr_t` + kernel.cpp `call_frame_push/pop`)**
   - 退避先はスレッド自身のスタック。`[ヘッダ][fp16(任意)][元の例外フレーム]` を
     `[X-total, X)` に置き、書き換え用フレームをその下に複製
   - **I-3: 元の例外フレームは 1 バイトも動かさない** (FP 拡張 S0-S15 との乖離防止)
   - **I-4: 書き換え用フレームは `snap - fs - 8`**。−8 は xPSR bit9 (SP 8B 整列
     パディング) の復帰時 +4 を見込む余裕。これを削ると無言ロックアップ (実際に踏んだ)
   - **I-5: pop は push 時に記録した total_bytes/frame_bytes を読み戻す** (再計算禁止)
   - PSPLIM 手前 64B で push を拒否 → panic せず NO_STACK エラー
2. **CTX_SAVE/CTX_RESTORE (svc_asm_handler.S)**
   - context_t オフセット (sp=32, exc_return=36, fp=40, psplim=104, control=108) を
     .equ と static_assert で両側から縛る手法
   - CONTROL は MRS→bic→orr→MSR の RMW で **nPRIV ビットだけ**差し替え (SPSEL/FPCA 温存)
   - 遅延 FP: EXC_RETURN bit4 で判別し S16-S31 のみ手動退避
3. **grant (時限実行権委譲)**: per-core grant_stack、期限は外側 deadline でクランプ
   (I-7)、tickless SysTick ワンショット + 最低優先度 PendSV で回収、
   SWITCH_THREAD インターセプトで早期復帰 (YIELDED)。budget=0 スレッドだけがバトン組
4. **claim CAS (`try_claim` / `arch_claim_thread_slot`)**: READY→RUNNING /
   UNINITIALIZED→RESERVED の CAS が SMP 安全性の根。**arch の継ぎ目として括り出し済み**
   (RP2350=LDREX / RP2040=SIO spinlock で真逆。DESIGN §14.5.3)
5. **例外優先度 = 同期規約**: SVC(0x00) > SysTick(0x40) > PendSV(最低)。
   「svc ハンドラ内 = そのコアでスレッド切替なし」を無償で得る (DESIGN §14.5.1)。
   **ARM 固有の前提なので arch 要件として明記すること**
6. **create_thread の READY 最終公開** (release store) + 生成時 affinity/budget 確定
   (後から set すると別コア claim との隙間レース)
7. **MPU Step0 (W^X)**: region0=XIP RO+X / region1=`__end__`〜SRAM_END RW+XN /
   PRIVDEFENA=1。実機合格。カーネル簿記 (context pool 等) を `__end__` より前の .bss に
   置き「非特権に渡す region から構造的に外す」手法も込み
8. **トランポリン ABI**: 一般オブジェクトの svc → フレーム push → pc をハンドラへ、
   r4=svc番号 / r5=発行元obj / r6=発行元thread / **r7=今のネスト数** (METHOD_EXIT の
   arg3 検算に使う両側チェック)。naked シム svc_handler_shim<F> が第 5-8 引数へ変換
9. **診断インフラ**: panic ring (noinit RAM, per-core, 記録して**戻る** = 系を殺さない)、
   svc_trace (noinit リング。無言ロックアップ後のブートで直近 svc が読める)、
   exit 検算失敗スレッドの隔離 (parked + yield ループ)

## 4. 参照コードの既知欠陥 (移植してはいけない部分)

DESIGN §12 が正規リスト。要点:

- **METHOD_CALL が呼び出し元 identity を潰す** (callee の r0 が常に呼び出し元でなく、
  カーネルオブジェクト経由だと 0 に化ける) → 新設計は `CALL(entry, callee_cookie,
  caller_cookie, protection, args…)` で cookie をカーネルオブジェクトが明示的に渡す
- **親子関係が無い** (誰でも誰でも呼べる) → デフォルト拒否が存在しない
- **md_table が引数索引** (誰でも他人の md を書き換え可能)、method_id 未実装、利用者ゼロ
  → 「呼び出し元索引 + 特権オブジェクトのみ払い出し + デフォルト拒否の後」で作り直し。
  **現行 md 実装は移植禁止** (PORT §8-8)
- **obj_api を r4==100/255 の臨時ハックで分岐** している箇所 (kernel_object.cpp) は負債
- **カーネルが object_table を直接持つ**構造そのもの (PORT §3 で externalize が決定)
- 固定 `[128]` 幅の表 (object_table 68.5KB 等)。新リポジトリは最初から疎に (PORT §5.3)
- カーネルオブジェクトから obj_api 番号を撃つと default で黙殺される穴 (→ 未知番号は
  必ずエラー返却。DESIGN §11.2.0)

## 5. ビルド (参照側を動かしたい場合)

```
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/14_2_Rel1
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cd latest_ver_from_flight_robocon/flight_robocon_telemetory_sender
cmake -S . -B build && cmake --build build -j8   # → build/main.uf2
```
トグル群は `include/kernel.hpp` 冒頭 (SHIZU_*)。既定で委譲 ON / MPU W^X ON / 300MHz。
pico2 (無印) 向けは `-DSHIZU_PICO2_TEST=1` (BLE 除外)。
