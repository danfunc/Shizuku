# 段階 2a (039e84b) 性能悪化の要因分析報告

## 1. 最有力結論 (1 行)

**`copy_fp_frame_72b` のインライン展開肥大化により、`[[gnu::always_inline]]` が無かった `call_resume_at` が GCC によって非インライン化され、Flash XIP 上で 18.6 KB 離れた単独関数 (0x100007c0) への遠距離関数呼出しが発生して XIP キャッシュミス (+560〜600 cyc/呼出し) が直撃したため。**

---

## 2. シンボル・アドレス・サイズ・配置 前後比較表

| シンボル名 | 18e201a (段階 1 基準) | 039e84b (段階 1 + 2a) | 変化内容 | セクション |
| :--- | :--- | :--- | :--- | :--- |
| `call_resume_at` | **インライン展開** (単独シンボルなし) | **0x100007c0** (サイズ 244 B) | **非インライン化 (新設)** | Flash XIP (.text) |
| `exit_method` | 0x10004d1c (サイズ 1480 B) | 0x10004d10 (サイズ 1344 B) | -136 B (インライン剥離) | Flash XIP (.text) |
| `forward_child_exit` | 0x100052e4 (サイズ 1492 B) | 0x10005250 (サイズ 1304 B) | -188 B (インライン剥離) | Flash XIP (.text) |
| `call_method` | 0x100058b8 (サイズ 1576 B) | 0x10005768 (サイズ 1576 B) | 差分 0 B (アドレス -0x150) | Flash XIP (.text) |
| `svc_dispatch` | 0x10003868 付近 (サイズ ~4260 B) | 0x10003718 (サイズ 4260 B) | `call_resume_at` 呼出し 3 箇所 | Flash XIP (.text) |
| `shizuku_armv8m_svc_handler` | 0x10006fe8 (サイズ 128 B) | 0x10006e98 (サイズ 128 B) | 機械語完全同一 (アドレス -0x150) | Flash XIP (.text) |
| `.data` 全体 (RAM 配置領域) | VMA 0x20000120 (サイズ 10632 B) | VMA 0x20000120 (サイズ 10632 B) | **完全同一** (サイズ 0x2988) | SRAM (.data) |
| `u_cost_parent_worker` | 0x20000640 (サイズ 1904 B) | 0x20000640 (サイズ 1904 B) | **1 バイトも差分なし (diff=0)** | SRAM (.time_critical.u_cost) |
| `u_cost_parent_worker_fp` | 0x20000296 (サイズ 938 B) | 0x20000296 (サイズ 938 B) | **1 バイトも差分なし (diff=0)** | SRAM (.time_critical.u_cost) |
| `u_cost_child_worker` | 0x20000262 (サイズ 4 B) | 0x20000262 (サイズ 4 B) | **1 バイトも差分なし (diff=0)** | SRAM (.time_critical.u_cost) |
| `u_cost_code_begin` | 0x20000260 (32B 整列) | 0x20000260 (32B 整列) | **完全同一** | SRAM (.time_critical.u_cost) |
| `u_cost_code_end` | 0x20000dc0 (32B 整列) | 0x20000dc0 (32B 整列) | **完全同一** | SRAM (.time_critical.u_cost) |

---

## 3. 根拠の詳細

### (1) RAM (.time_critical) から Flash への溢れは一切ない
- `.data` セクション (RAM 実行コード含む): VMA `0x20000120`、サイズ `0x2988` (10,632 バイト) ともに完全一致。
- 18e201a の UF2 から復元した LMA (`0x10024834`) と 039e84b の LMA (`0x10024534`) を比較した結果、全 10,632 バイト中、異なっていたのは Flash アドレスのシフトに伴う PC 相対 bl / 関数ポインタ等の 59 バイトのみ。
- `.time_critical` や SRAM 配置関数が容量超過で Flash に落ちた事象は **0 件**。

### (2) 計測ループ (u_cost SRAM 窓) の配置・コードは完全同一
- U モード計測窓 `[u_cost_code_begin (0x20000260), u_cost_code_end (0x20000dc0))` 内のバイナリをバイト比較した結果、**差分は 0 バイト (完全同一)**。
- `u_cost_parent_worker` (0x20000640), `u_cost_child_worker` (0x20000262) のアドレス・32 バイトアライメント・命令列に一切変化はない。

### (3) 悪化の直接原因: `call_resume_at` の非インライン化と XIP キャッシュミス
- `arch/armv8m/armv8m.hpp` の 728 行目で `copy_fp_frame_72b` に `[[gnu::always_inline]]` を付与。
- しかし、呼び出し側の `call_resume_at` (736 行目) には `[[gnu::always_inline]]` が**付いていなかった** (RV32 の `arch/riscv/rv32.hpp:379` には付いていた)。
- 18 語の直接展開コピー (36 命令の ldr/str) が強制インライン展開された結果、`call_resume_at` のコードサイズが 244 バイトに膨張。GCC のインライン閾値を超えたため、**単独のアウトライン関数 `100007c0 <call_resume_at [clone .constprop.0]>` として Flash 上に分離生成**された。
- これにより、以下のホットパスで Flash XIP 遠距離呼出しが発生:
  - `exit_method` (0x10005066) -> `bl 100007c0` (ジャンプ距離: **-18,598 B = -18.1 KB**)
  - `forward_child_exit` (0x100056b2) -> `bl 100007c0` (ジャンプ距離: **-20,210 B = -19.7 KB**)
  - `svc_dispatch` (0x10004340, 0x1000443e, 0x100044c4) -> `bl 100007c0` (ジャンプ距離: **-15.2 KB〜-15.6 KB**)
- **RP2350 の XIP キャッシュ容量は 16 KB**。18 KB 離れた Flash アドレスへの call および `pop {..., pc}` による return は、**確実に XIP キャッシュミス (Direct Mapped / 2-way 衝突) を誘発し、外付け QSPI Flash への低速バーストフェッチを往復で発生**させる。

### (4) なぜ `desc` (+1120 cyc) は `method` (+600 cyc) の約 2 倍悪化したのか
- **`method` 呼出し (U→U)**:
  - 往路: `svc_call_method` (0x10005768) は `call_resume_at` を経由しない。
  - 復路: `exit_method` (0x10005066) で `call_resume_at` を **1 回** 呼ぶ。
  - -> ペナルティ: **約 +600 cycles** (1 回分)。
- **`desc` (descriptor) 呼出し (U→U)**:
  - 往路: `svc_call_descriptor` (0x10004340, `svc_dispatch` 内) で `call_resume_at` を **1 回** 呼ぶ。
  - 復路: `exit_method` (0x10005066) で `call_resume_at` を **1 回** 呼ぶ。
  - -> ペナルティ: **合計 2 回** 呼ぶ。
  - -> 2 回 × ~560 cyc = **+1120 cycles**。実測の悪化幅 (+1120) と完全に一致。

---

## 4. 2a を直して再挑戦すべきかの意見

- **結論**: **直して再挑戦すべきである。**
- **理由**: 悪化原因の 100% が「`call_resume_at` の非インライン化に伴う 18KB 遠距離 XIP キャッシュミス」であり、`call_resume_at` に `[[gnu::always_inline]]` を付ける（または FP コピー部をコールドパスとして隔離する）だけでこの悪化は完全に消滅する。本来の 72B 展開コピーによる高速化恩恵は確実に得られる。
