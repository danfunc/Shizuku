#set document(title: "Shizuku V3 Object Method-Call API 設計前調査報告書", author: "下層 Gemini 調査")
#set page(paper: "a4", margin: (x: 18mm, y: 18mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8.5pt, lang: "ja")
#set par(leading: 0.6em, justify: true)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 7.2pt)
#let ref_meta(s) = text(size: 7.5pt, fill: rgb("555555"), s)

= Shizuku V3 Object Method-Call API 設計前調査報告書

#ref_meta([状態: 第2稿 (一次レビュー差戻し対応版)。日付: 2026-10-08。対象リポジトリ: Shizuku-stream (commit: `3239d3761fd1898b78411434a31f91790251bd3b`)。])

== 調査目的・前提と位置づけ

=== 調査の背景と目的
Shizuku OS はマイクロ組み込み環境において、実行権貸付け (GRANT/YIELD) と保護ドメイン・オブジェクトランド構造を備えた高信頼リアルタイムカーネルの開発を進めている。現行 V2 実装では、オブジェクトランドのハンドラ (`source/kernel_object/handler.cpp`) を介してメソッド呼び出しが実現されているが、その呼出し引数はスカラー 1 語 (`args[0]`) に限定され、可変長通信や構造化データ授受は別系統のストリーム API や共有メモリに依拠している。
本調査は、次世代 Shizuku V3 におけるオブジェクトメソッド呼出し API (`object method-call API`) の設計に向け、先行・関連 OS の設計思想と実装を公式一次資料および実コードに基づき比較・体系化することを目的とする。本書は設計決定を行わず、選択肢・論点・未決事項の提示に徹する。

=== 調査方針と事実・推論・未確認の分離規範
本報告書では、以下の規範に厳格に従い記述する:
- *確定事実*: 実機/実コード/コミット固定 URL/公式一次仕様で直接確認された事項 (file:line, URL, hash を付記)。
- *設計意図 / 提案*: 設計書で述べられている方向性や仮説 (未実装または検討中のもの)。
- *未確認 / 不明 / 規定しない*: 調査範囲内で一次資料から裏取りが取れなかった項目、または仕様・カーネルプリミティブが規定していない方針 (明示的に「未確認」「規定しない」と記載)。
- *中立性*: 特定のアーキテクチャへの断定的誘導や Shizuku 側の設計決定は行わず、候補・選択肢・トレードオフ・未決事項の提示に徹する。

=== Shizuku 確定前提と現行コードとの対比
発注元指示および現行コードに基づき、以下の前提事実を確認・整理した:
+ *`source/kernel_object/handler.cpp` の実装状況*:
  - L318–325: `current_object(thread)`。特権台帳 (`call_ledger`) を走査し、最新の `LEDGER_CALL` エントリの `object` を返す。ネスト呼び出しが存在しない場合はスレッド生成時の `m_thread_object[thread]` を返す。
  - L327–335: `caller_object(thread)`。台帳を走査して 2 番目の `LEDGER_CALL` エントリの `object` を返し、単一呼出し時は `m_thread_object[thread]`、それ以外は `NO_OBJECT` を返す。
  - L337–355: `interposed_handler(thread)`。台帳上の 0 番エントリが root (`KERNEL_OBJECT_ID`, `LEDGER_DISPATCH`)、1 番エントリが専用ハンドラ (`object_kind::HANDLER`, `LEDGER_DISPATCH`)、2 番エントリが子オブジェクト (`LEDGER_CALL`) であることを検証し、さらに `m_objects[child].parent_handler_object == handler_id` であることを照合する。
  - L533–581: `call_method(id, method, argument, error)`。`call_request` 構造体 (`entry_pc`, `callee_object`, `parent_handler_object`, `protection`, `region_base`, `region_limit`) を構築し、L567 で `request.args[0] = argument` を設定後、L570–571 で `ARCH::syscall((uintptr_t)primitive::CALL, (uintptr_t)&request)` を発行する。
  - L620–648: `forward_child_exit(value, error)`。L626–630 で `interposed_handler(thread)` と `svc_permission_allows` により発行元が真正な親ハンドラであることを検査し、L637–645 で `ARCH::syscall((uintptr_t)primitive::RETURN, count=3, value, error, cur_depth)` を発行して Root + Handler + Child の 3 フレームを一括巻き戻す。
  - L1572–1690: `KERNEL_OBJECT::handle(number, a1, a2, a3)` エントリ。L1589–1606 において `CALL_METHOD`, `EXIT_METHOD`, `FORWARD_CHILD_EXIT`, `GET_CURRENT_OBJECT`, `GET_CALLER_OBJECT` 等をディスパッチする。
  - *identity の信頼根拠*: 呼出し元および現在オブジェクトの特定は、呼出し側が引数で名乗る値を信用するのではなく、カーネルが管理する特権台帳 (`call_ledger`) の走査結果から導出される点に限定して保証される。
+ *`docs/design/kernel_v3.typ` の設計方向と表記の相違*:
  - L20, 34–37, 76–80, 89–100 を確認。
  - *静的属性集合の表記相違*: 発注元が指定した静的属性集合は `{permission, from_isr, mode, save_level}` であるのに対し、現行文書 L35 では `(svc_attributes: permission, from_isr, mode, save_level, authority)` と `authority` を含む 5 属性が記述されている。本報告書ではこの相違を事実として記録し、どちらを採用するかの判断・設計決定は行わない。
  - *variable-length stream の記載状況*: 発注元より V3 の方向性として提示されたが、調査対象である現行 `docs/design/kernel_v3.typ` の本文中には直接の該当文字列は確認されなかった (本調査対象の現行文書内では直接記載は未確認。ユーザー指定の設計方向として整理)。
  - context reuse によるスタック幾何・保護境界分割、実行権の GRANT 貸付けと無引数 YIELD 返却、ドライバ API を user/kernel 双方から呼出可能とし peripheral object が interface 差を吸収する方針が明記されている。
+ *`docs/record/kernel_v2_record.typ` の不在と代替資料*:
  - 指定されたパス `docs/record/kernel_v2_record.typ` はリポジトリ内に存在しない (`docs/record/` ディレクトリ自体が存在しない)。
  - 代替関連資料として `docs/design/kernel_stream.typ` が存在する。`docs/01_current_repo_survey.md`、`docs/02_reference_survey.md` は後に削除した (要約は `docs/record/legacy_docs_summary.typ`)。

== Scarlet 詳説 (Deep Dive)

Scarlet は Rust で書かれたマルチ ABI 対応のオペレーティングシステムであり、共有カーネルオブジェクト上に native, Linux, xv6 等の ABI モジュールを構築する設計を特徴とする。
- *クローン先*: `/private/tmp/shizuku-survey/Scarlet`
- *採用コミット*: `0842752a2304225ab7b78a37757c9575a94195ae` (branch: `dev`, working tree clean)
- *固定コミット URL*: #link("https://github.com/petitstrawberry/Scarlet/tree/0842752a2304225ab7b78a37757c9575a94195ae")[petitstrawberry/Scarlet (commit 0842752)]

=== レイヤ構造と責務境界
Scarlet のネイティブ API スタックは、以下の 4 層で厳密にレイヤ化されている (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/docs/release/1.0-native-api-contract.md#L18-L26")[1.0-native-api-contract.md:18–26]):
+ *`scarlet-abi`*: 依存なし `no_std` のシステムコール番号、定数、ABI 構造体定義。
+ *`scarlet-sys`*: 生の `unsafe` システムコール呼出しエントリ (`syscall0` 〜 `syscall6`)。カーネルの生戻り値をそのまま返し、単一の errno 体系を強制しない。
+ *`scarlet-os`*: 型付き `Handle`、ケイパビリティビュー、高水準抽象 (IPC, socket, poll, process, scheduler 等)。
+ *`scarlet-rt`*: プロセスエントリ、終了、アロケータ glue。

=== システムコール構成と KernelObject 抽象
カーネル側のシステムコール (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/syscall/mod.rs#L295-L305")[kernel/src/syscall/mod.rs:295–305]) は、機能レンジごとに体系化されている:
- *100–109*: ハンドル管理 (`HandleQuery=100`, `HandleSetRole=101`, `HandleClose=102`, `HandleDuplicate=103`, `HandleGetFlags=104`, `HandleSetFlags=105` 等)
- *110*: `HandleControl` (デバイス/オブジェクト制御、ioctl 相当)
- *200–204*: `StreamOps` capability (`StreamRead=200`, `StreamWrite=201`, `Poll=202` 等)
- *300–310*: `FileObject` capability (`FileSeek=300`, `FileTruncate=301`, `FileMetadata=302`, `FileLock=310` 等)
- *400–418*: VFS 操作 (`VfsOpen=400`, `VfsRemove=401`, `VfsOpenAt=417` 等)
- *600–644*: IPC 操作 (`Pipe=600`, `SharedMemoryCreate=620`, `SocketSendHandle=630`, `SocketRecvHandle=631`, `EventHandlerRegisterNative=640` 等)
- *900–921*: Socket / ネットワーク操作

カーネル内の資源は `KernelObject` enum (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/mod.rs#L32-L53")[kernel/src/object/mod.rs:32–53]) として統一表現される:
```rust
pub enum KernelObject {
    Environment(EnvironmentHandle), VfsView(ViewHandle),
    File(Arc<dyn FileObject>), Pipe(Arc<dyn PipeObject>),
    Counter(Arc<dyn CounterObject>), Timer(Arc<dyn TimerObject>),
    EventChannel(Arc<EventChannelObject>), EventSubscription(Arc<EventSubscriptionObject>),
    Socket(SocketHandle), SharedMemory(Arc<dyn SharedMemoryObject>),
    Gpu(Arc<dyn GpuObject>), HypervisorVm(VmRef), HypervisorVcpu(VcpuRef),
}
```
各オブジェクトはケイパビリティトレイト (`StreamOps`, `FileObject`, `ControlOps`, `Selectable`, `MemoryMappingOps`, `CloneOps`) を実装する。

=== Handle 管理・Introspection・unsafe 境界
ユーザ空間における資源の所有権は `scarlet-os::Handle` (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L78-L82")[user/lib/scarlet-os/src/handle/mod.rs:78–82]) が唯一所有する。
- *Introspection*: `Handle::open` や `Handle::from_raw` の生成時、カーネルに `Syscall::HandleQuery` (100) を発行し、`KernelObjectInfo` (オブジェクト種別、サポートする capability ビットフラグ) を取得して構造体内にキャッシュする (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L88-L104")[handle/mod.rs:88–104])。
- *Capability View の借用*: `Handle::as_stream()`, `as_file()`, `as_socket()`, `as_shared_memory()`, `as_memory_mapping()` は、キャッシュされたメタデータを検証し、ライフタイム借用ビュー (例: `StreamOps<'_>`) を返す (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L217-L278")[handle/mod.rs:217–278])。
- *所有権と Drop*: `Handle` の `Drop` 実装は `HandleClose` (102) を確実に 1 度呼び出す (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L332-L340")[handle/mod.rs:332–340])。
- *Unsafe 境界*: 生ハンドル構築 (`Handle::from_raw`) や `Handle::control` は `unsafe`。特に `control(command: u32, arg: usize)` は任意のポインタや構造体を渡しうるため、呼出し側がポインタ有効性・ライフタイム・アライメントを保証する義務を負う (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L281-L315")[handle/mod.rs:281–315])。

=== メソッド呼び出しの実態: HandleControl とユーザ空間 RPC (sbus)
Scarlet のメソッド呼出しアーキテクチャを実コードから精査した結果、以下の重要事実が判明した:
+ *カーネル層の制御呼出し (`sys_handle_control`)*:
  #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/handle/syscall.rs#L203-L223")[kernel/src/object/handle/syscall.rs:203–223] において、`sys_handle_control` は引数 `(handle: u32, command: u32, arg: usize)` を受け取る。doc コメントには `arg: Command-specific argument (often a pointer to data)` と記述されている通り、引数型自体は `usize` スカラーであり、ポインタを渡すこともあればスカラ値を渡すこともある (例: ノンブロッキング設定)。L250–289 の #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/handle/syscall.rs#L279-L289")[L279–289] で `control_ops.control(command, arg)` へディスパッチする。カーネル内部にはメソッド IDL や型付きディスパッチャは存在せず、POSIX の `ioctl` と同等の汎用制御機構である。
+ *ユーザ空間 RPC / メソッド呼出し (`sbus`)*:
  オブジェクト指向の高水準メソッド呼出しは、カーネル外のユーザ空間ライブラリ `sbus` (Scarlet Bus; #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/lib.rs#L1-L32")[user/lib/sbus/src/lib.rs:1–32]) および `sbus-client` (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus-client/src/lib.rs#L181-L199")[user/lib/sbus-client/src/lib.rs:181–199]) が担当している。
  - *トランスポート*: Unix ドメインソケット (`/tmp/sbus.sock`)。
  - *メッセージ型*: `Message::CallMethod { destination, path, interface, method, args: Vec<Argument> }` (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/message.rs#L57-L72")[user/lib/sbus/src/message.rs:57–72])。
  - *引数型*: `Argument::String(String)`, `Argument::Int(i32)`, `Argument::UInt(u32)`, `Argument::Boolean(bool)` (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/types.rs#L7-L14")[user/lib/sbus/src/types.rs:7–14])。
  - *同期呼出し*: `Connection::call_method` (#link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus-client/src/lib.rs#L181-L199")[sbus-client/src/lib.rs:181–199]) は要求フレーム送信後、`wait_for_method_response` で `serial` が一致する `MethodReturn` または `MethodError` を受信待機する。
+ *設計文書の主張と実装の一致 / 差異*:
  - *一致*: `docs/release/1.0-native-api-contract.md` の所有権モデル、Drop 規約、借用ビュー、生 control の unsafe 義務付けは実装と完全一致。
  - *限定的結論*: カーネルの IPC / オブジェクトプリミティブ自体は細粒度なメソッド IDL を含まず、大分類システムコールと `HandleControl` (ioctl 相当) を提供する。メソッド呼出しプロトコルはユーザ空間ソケット通信 (`sbus`) として分離されている。

== 比較対象 OS の調査

公式一次資料に基づく各 OS のメソッド呼出し / IPC アーキテクチャの調査結果を示す。

=== seL4
#ref_meta([一次資料: seL4 Reference Manual (https://sel4.systems/Info/Docs/seL4-manual-latest.pdf#page=31), seL4 IPC Tutorial (https://docs.sel4.systems/Tutorials/ipc.html)])
seL4 は形式検証された L4 系マイクロカーネルであり、資源は CSpace 内の CPtr で参照される。
- *呼出しプリミティブ*: `seL4_Call(dest: seL4_CPtr, msgInfo: seL4_MessageInfo_t) -> seL4_MessageInfo_t`。同期送信を行い相手の返信までブロック。カーネルが受信側 TCB にワンタイム Reply ケーパビリティを自動装填する。
- *引数渡し*: メッセージレジスタ (MR0〜MRn) による直接レジスタ転送。超過分はスレッド固有の IPC Buffer (4 KiB ページ内) を使用。
- *型安全と権限 (機構と政策の分離)*: カーネルプリミティブは型を知らず、`seL4_CapRights` (Read/Write/Grant) と 32/64-bit `Badge` (送信元認証) のみを提供。メソッド単位の型付けやディスパッチ認可は CAmkES IDL や Microkit 等の上位フレームワークの責務である。

=== Fuchsia / Zircon
#ref_meta([一次資料: Zircon Syscall channel_call (https://fuchsia.dev/reference/syscalls/channel_call), FIDL Overview (https://fuchsia.dev/development/languages/fidl/overview)])
Google のケイパビリティベース OS。カーネル核 Zircon はオブジェクトとハンドル (`zx_handle_t`) を提供する。
- *呼出しプリミティブ*: `zx_channel_call(handle, options, deadline, args, &actual_bytes, &actual_handles)`。同期送信・返信待機・受信をアトミックに実行。
- *引数渡し*: メッセージバッファ (最大 64 KiB) とハンドル配列 (最大 64 個) を一括転送。
- *型安全と権限 (機構と政策の分離)*: カーネルは `ZX_RIGHT_READ`, `ZX_RIGHT_WRITE` 等のチャネル全体権限を管理する。メソッド識別子 (`ordinal`) や要求相関 (`txid`)、引数シリアライズは言語非依存 IDL (FIDL) が生成するユーザランド規約である。完全非同期 RPC は `zx_channel_write` と Port 待機で実現される。

=== Genode OS Framework
#ref_meta([一次資料: Genode OS Foundations 24.05 (https://genode.org/documentation/genode-foundations-24-05.pdf#page=125), RPC Tutorial (https://genode.org/documentation/developer-resources/client_server_tutorial)])
マイクロカーネル上にコンポーネントツリーを構築するオブジェクト指向 OS フレームワーク。
- *呼出しプリミティブ*: `Capability<T>` 型による型付き RPC 参照 (`cap->method(args...)`)。
- *言語統合型 IDL*: 外部 IDL コンパイラを用いず、C++ テンプレートとマクロ (`GENODE_RPC`, `GENODE_RPC_INTERFACE`) によるコンパイル時メタプログラミングで型安全なシリアライズコードを生成。
- *引数渡しと権限*: 下位カーネル (seL4 等) の IPC バッファを使用し、大容量データは `Dataspace` (共有メモリ) を受け渡す。メソッド単位権限は独立インターフェース Capability の分割設計によって表現される。

=== Hubris
#ref_meta([一次資料: Hubris Reference Manual (https://github.com/oxidecomputer/hubris/blob/master/doc/reference.adoc), Oxide Computer Company])
Oxide Computer Company による Cortex-M 等の組込みマイコン向け高信頼 OS (Rust 製)。
- *呼出しプリミティブ*: `sys_send(task, op, send_slice, recv_slice)` による同期送信・返信待機。動的タスク生成を排し、タスク ID はビルド時に静的固定。
- *メモリ渡し (Memory Lease)*: 小容量メッセージはメモリ間コピーで転送。大容量バッファはトランザクション中のみ受信側へアクセス権を一時貸与する Memory Lease 機構を用いる (完全ゼロコピーではなく、コピーと貸与を使い分ける)。
- *IDL*: Idol (`.idol` / Rust proc-macro) により型安全なクライアントスタブとサーバディスパッチャを自動生成。

=== Redox OS
#ref_meta([一次資料: Redox OS Book Schemes (https://doc.redox-os.org/book/ch04-03-schemes.html), Redox syscall repository (https://gitlab.redox-os.org/redox-os/syscall)])
Plan 9 思想を継承した Rust 製マイクロカーネル。「全ては URL / スキームである」を基本設計とする。
- *呼出しプリミティブ*: POSIX 風のファイル記述子と `open`, `read`, `write`, `fevent`。明示的なカーネル RPC プリミティブは持たない。
- *スキーム機構*: サーバは Scheme ハンドラとして常駐し、カーネルから `Packet { id, op, a, b, c }` 構造体を受信して処理・返信。型付けはカーネルレベルでは存在せず、バイト列シリアライズに依存。

=== L4Re / Fiasco.OC
#ref_meta([一次資料: L4Re IPC API (https://l4re.org/doc/group__l4__ipc__api.html), L4Re C++ Interface (https://l4re.org/doc/group__l4__cxx__ipc__ifaces.html)])
L4 マイクロカーネル Fiasco.OC 上のオブジェクト指向ランタイム環境。
- *呼出しプリミティブ*: `l4_ipc_call(dest_cap, utcb, msgtag, timeout) -> l4_msgtag_t`。
- *引数渡し*: UTCB (User Thread Control Block) 内のメッセージレジスタ (MR) およびバッファレジスタ (BR) を使用。
- *オブジェクトモデル*: C++ 層で `L4::Kobject` を基底とし、`L4_RPC`, `L4_INLINE_RPC` マクロで型安全な C++ RPC インターフェースを構築。

=== QNX Neutrino RTOS
#ref_meta([一次資料: QNX Neutrino Architecture IPC (https://www.blackberry.com/us/en/products/qnx-software-development-platform/qnx-neutrino-rtos), QNX Message Copying (https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/ipc_Message_copying.html)])
商用リアルタイムマイクロカーネルの標準的実装。
- *呼出しプリミティブ*: `MsgSend(coid, smsg, sbytes, rmsg, rbytes)`。同期クライアント・サーバ IPC。
- *引数渡しとコピー特性*: `MsgSendv`, `MsgReceivev` により `iovec` (`iov_t { iov_base, iov_len }`) 配列を用いた scatter-gather 転送をサポート。送信側での連続バッファへの事前結合コピーは回避されるが、カーネルによるアドレス空間間のデータコピーは行われ、short-message optimization では中間バッファも用いられる (完全ゼロコピーではない)。

== 網羅的比較表

=== 表 1: 基本呼出し規約・データ転送比較
#text(size: 7.2pt)[
#table(
  columns: (1fr, 1.3fr, 1.4fr, 1.5fr, 1.3fr, 1.4fr, 1.2fr),
  inset: 3pt,
  table.header(
    repeat: true,
    [対象 OS], [宛先識別子], [method 番号 / IDL], [引数受渡方式], [可変長データ], [return / error], [async (非同期)]
  ),
  [Scarlet],
  [Handle (u32)],
  [ioctl 番号 (u32) / ユーザ層 sbus],
  [スカラー (u32/usize) またはソケット],
  [Stream / SHM / sbus パケット],
  [i32 / errno / sbus Error],
  [Poll / Event / sbus 非同期],

  [seL4],
  [CPtr (Endpoint)],
  [label (語内) / CAmkES IDL],
  [レジスタ (MR0..n) + IPC Buffer],
  [IPC Buffer (頁内) / 共有頁],
  [MessageInfo + Reply cap],
  [Notification (シグナル)],

  [Fuchsia],
  [zx_handle_t (Channel)],
  [ordinal (u64) / FIDL],
  [インライン byte 配列 + handle 配列],
  [最大 64KiB / VMO (共有メモリ)],
  [txid 相関メッセージ],
  [Channel + Port (第一級)],

  [Genode],
  [`Capability<T>`],
  [RPC ID (u32) / GENODE_RPC],
  [IPC バッファ / 引数ストリーム],
  [Dataspace (共有メモリ)],
  [C++ 戻り値 / RPC 例外],
  [非同期 Signal / RPC 原則同期],

  [Hubris],
  [Task ID (静的)],
  [op (u16) / Idol IDL],
  [スライス参照 + Memory Lease],
  [Memory Lease (アクセス権貸与)],
  [`Result<usize, ErrorCode>`],
  [通知 (Notification) / 原則同期],

  [Redox],
  [fd (スキーム)],
  [op (Packet) / URL スキーム],
  [Packet 構造体 (スカラー 3 語)],
  [read/write バイトストリーム],
  [isize / errno],
  [fevent (イベントキュー)],

  [L4Re],
  [l4_cap_idx_t],
  [protocol+opcode / L4_RPC],
  [UTCB (MR レジスタ群)],
  [UTCB バッファ / 共有メモリ],
  [msgtag + 戻り値],
  [IPC timeout / Irq ハンドラ],

  [QNX],
  [coid / chid],
  [type/subtype (ヘッダ内)],
  [iovec (scatter-gather)],
  [MsgSendv (複数 iov)],
  [MsgReply ステータス値],
  [Pulse (非同期固定長 64bit)]
)
]

=== 表 2: 型安全・権限・ライフサイクル比較
#text(size: 7.2pt)[
#table(
  columns: (1fr, 1.4fr, 1.5fr, 1.5fr, 1.1fr, 1.4fr),
  inset: 3pt,
  table.header(
    repeat: true,
    [対象 OS], [IDL / codegen / 型安全], [権限 / 委譲 / 取消], [親子 / 終了・障害伝播], [両用 driver API], [代表プリミティブ]
  ),
  [Scarlet],
  [Rust トレイト (カーネル無関知)],
  [所有権移動 / Duplicate / 取消不可],
  [Task Namespace / 親プロセス],
  [可能 (VFS/Handle 経由)],
  [`HandleControl` / sbus],

  [seL4],
  [CAmkES / Microkit (上位層)],
  [CNode / Mint / Badging / Revoke],
  [規定しない (ポリシー自由)],
  [可能 (Endpoint 統一)],
  [`seL4_Call` / `ReplyRecv`],

  [Fuchsia],
  [FIDL (静的 codegen)],
  [Rights / Replace 削減 / 取消不可],
  [Component Framework / 親子ルーティング],
  [可能 (Channel 統一)],
  [`zx_channel_call`],

  [Genode],
  [C++ テンプレート / 型メタプロ],
  [Capability 委譲 / 取消支援],
  [Parent-Child 木 / 資源クォータ監督],
  [可能 (Session 抽象)],
  [`cap->method()` RPC],

  [Hubris],
  [Idol (Rust proc-macro)],
  [静的呼出し権限 / 動的取消なし],
  [Supervisor / 死活監視・タスク再起動],
  [可能 (Task IPC 統一)],
  [`sys_send` / Lease],

  [Redox],
  [なし (バイト列シリアライズ)],
  [fd 所有権 / dup / 取消不可],
  [POSIX 風プロセス木],
  [可能 (Scheme 経由)],
  [`read` / `write` / Packet],

  [L4Re],
  [L4_RPC / C++ クラス],
  [Capability / Task CSpace / Unmap],
  [Parent-Child / 環境管理],
  [可能 (Kobject 経由)],
  [`l4_ipc_call`],

  [QNX],
  [なし (C 構造体手動定義)],
  [チャネル権限 / POSIX 権限],
  [プロセスグループ / 障害シグナル],
  [可能 (MsgSend 統一)],
  [`MsgSend` / `MsgReply`]
)
]

== POSIX fd/read/write/ioctl 代替の論点整理

POSIX の伝統的な抽象 (`fd` に対する `read`/`write`/`ioctl`) は、UNIX の「全てはファイルである」という統一的パイプライン構造と単純性をもたらした一方で、組込み・マイクロカーネル環境におけるオブジェクト指向設計との間で明確なトレードオフが存在する。

=== POSIX 抽象の利点と限界
+ *抽象の簡潔さと型検査のトレードオフ*:
  - *利点*: 全ての資源をバイトストリームとして一様に扱い、標準ツール群やリダイレクトで容易に結合可能。
  - *限界*: `ioctl(int fd, unsigned long request, ...)` は C 言語レベルで可変長引数 (`...`) を許容するため、コンパイラによる静的型検査が効かない。各ドライバで個別 ABI 構造体を定義・検査する運用 (例: Linux V4L2 や DRM) が行われているが、言語レベルの型保証ではなく個別実装コードでのバリデーションに依存する。
+ *名前空間の管理*:
  - *利点*: ファイルシステムパスによる単一名前空間での資源解決。
  - *限界*: `ioctl` コマンド番号は magic number 慣習に頼るため、番号の衝突や、無効なコマンドが別ドライバで誤解釈されるリスクが構造的に残る。
+ *権限モデルの粒度*:
  - *利点*: `O_RDONLY`, `O_WRONLY`, `O_RDWR` の単純明快なアクセス制御。
  - *限界*: 「特定の設定取得は許可するが、パラメータ変更は拒否する」といったメソッド単位のきめ細かな権限管理をファイルディスクリプタ単体で表現することは困難。
+ *データコピーの特性*:
  - *限界*: 通常の `read`/`write` はカーネルとユーザ空間間のバッファコピーを前提とする。`mmap` や `splice` などのゼロコピー技術も存在するが、小規模マイコン (MPU 環境) では MMU を前提とする仮想記憶機構が使えず、直接適用が難しい場合がある。

=== 型付きオブジェクト / チャネル RPC の利点
+ *コンパイル時 / インターフェース時の静的型検証*:
  FIDL (Fuchsia) や `GENODE_RPC` (Genode)、Idol (Hubris) のように、インターフェースを明確に型定義することで、引数の型・数・方向 (in/out) がコンパイル時に静的検証される。
+ *細粒度なケーパビリティ管理*:
  エンドポイントやチャネル単位で独立したハンドル/ケーパビリティが発行され、権限削減 (Rights Reduction) やアクセス制限を分離できる。
+ *改ざん不能な呼出し元特定*:
  seL4 の `Badge` や Shizuku の特権台帳走査のように、呼出し側が自称する引数を信用せず、カーネル機構により安全に発信元を識別可能。

=== 導入に伴うトレードオフと複雑性 (Hypothesis 検証)
型付き RPC / オブジェクト呼出しへの移行には、以下のトレードオフが伴う:
+ *IDL / コード生成ツールの保守負担*:
  専用 IDL コンパイラを導入する場合、ホストビルド環境への依存が増大する。組込み環境では Genode (C++ テンプレート) や Hubris (Rust proc-macro) のように言語ネイティブ機能で完結させるアプローチが有力な選択肢となる。
+ *非同期呼出しの複雑性*:
  完全非同期を導入すると、要求と応答のライフタイム管理、タイムアウト処理、メモリバッファの回収責任が大幅に増大する。Hubris や QNX、seL4 のように「基本は同期 Call、通知のみ非同期」とする割り切りが組込みでは効果的である。
+ *障害伝播とクリーンアップ*:
  呼出し先がクラッシュまたは停止した場合に、呼出し元のブロックをいかに安全に解除し (例: seL4 の Reply cap 無効化、Hubris の `DeadTask` 返却)、貸与した資源を回収するかというライフサイクル設計が不可欠となる。

== Shizuku 既存コードとの対比と V3 への示唆・選択肢

=== 現行 V2 実装 (`handler.cpp`) の評価
- *到達点*:
  `handler.cpp` (L533–581) の `call_method` は、特権台帳 (`call_ledger`) を用いた確実なフレーム追跡、親ハンドラ (`parent_handler_object`) による多段委譲・終了転送 (`FORWARD_CHILD_EXIT`: L620–648)、および台帳に基づく identity 導出 (L318–335) を確立している。
- *課題・制約*:
  呼出し引数が `args[0]` (スカラー 1 語) に限定されており、複数引数や可変長引数を渡すには呼び出し側がポインタを詰めるか、別系統のストリーム (`STREAM_PUSH_N`) を経由する必要がある。またメソッド番号は整数値 (`uintptr_t method`) の直接指定であり、静的な型安全性が言語レベルで提供されていない。

=== V3 設計骨格 (`kernel_v3.typ`) との整合性
`docs/design/kernel_v3.typ` で示された V3 の基本方針と、オブジェクト呼出しの論点との関係を整理する:
+ *静的 API 属性表 (`svc_attributes`)*:
  L20, 35 で規定された属性表は、システムコール入口での静的ゲートとして機能する (発注元提示の 4 属性 `{permission, from_isr, mode, save_level}` と、現行ファイル表記の `authority` を含む 5 属性の相違は前述の通り事実として記録)。オブジェクトメソッド呼出しも、この属性表に基づき native レーン / objectland レーンの判定を受ける。
+ *context reuse とスタック幾何*:
  同一コンテキスト内で `call_frame_push`/`pop` を行う context reuse の方針は、seL4 の `seL4_Call` や Hubris の同期 IPC と同等の極小オーバーヘッドを目指すものである。
+ *GRANT / YIELD 実行権貸付け*:
  L34 で規定された「内側予算の外残量 clamp」と無引数 YIELD 返却は、同期メソッド呼出しにおける CPU 時間貸与と自然に調和する。呼出し元 (貸し手) が呼出し先 (借り手) に CPU 時間を貸し、メソッド終了 (`exit_method`) が貸し手への即時復帰となる。
+ *peripheral object によるインターフェース差の吸収*:
  L36, 80 で規定された「driver API を user/kernel 双方から呼出可能とし、kernel 側の interface 差は peripheral object が吸収する」方針は、Genode の Session 抽象や Fuchsia の Channel 統一モデルと親和性が高い。

=== Shizuku V3 における設計候補とトレードオフ (未決事項の提示)
本報告書は設計決定を行わず、以下の 4 つの候補案と未決事項を比較提示する:

#table(
  columns: (1fr, 1.8fr, 1.8fr, 1.8fr),
  inset: 3pt,
  table.header(repeat: true, [候補案], [概要・機構], [長所], [短所・課題]),
  [案 A: スカラー拡張 + ストリーム連携 (V2 発展型)],
  [レジスタ引数を 1 語から 4 語 (a0–a3) へ拡張。可変長・大容量データは既存の `STREAM_*` または共有アリーナで渡す。],
  [- 実装変更が最小限\ - カーネルの極小性を維持\ - 静的属性表と直結可能],
  [- 構造化データの型安全性が C++ 側に委ねられる\ - 引数が多い場合の詰替え負荷],
  [案 B: UTCB / メッセージバッファ方式 (seL4/L4Re 型)],
  [スレッドまたはコンテキストごとに固定サイズのメッセージ領域 (UTCB) を定義し、ヘッダ + 複数語をアトミック転送。],
  [- 多数の引数を一括転送可能\ - メッセージタグによる型分類\ - 標準的マイクロカーネル構造],
  [- UTCB のメモリ配置と MPU 保護管理の複雑化\ - ゼロコピーではない (バッファ往復)],
  [案 C: 借用型ゼロコピー IPC (Hubris 型)],
  [同期呼び出し中、呼出し元のスタック/バッファ領域を呼出し先ドメインへ MPU 窓として一時借用 (`borrow` / Lease) させる。],
  [- 可変長データの完全ゼロコピー転送\ - 動的メモリ確保が不要\ - マイコンの MPU と高親和],
  [- MPU リージョン数が少ないハードウェアでの制約\ - 入れ子呼出し時のリージョン枯渇],
  [案 D: ハンドル + ユーザ空間バス/IDL 分離 (Scarlet 型)],
  [カーネルは Handle と汎用制御/ストリームのみを提供し、高水準なメソッド呼出しはユーザ空間ライブラリ (D-Bus 類似) で解決。],
  [- カーネルが極めて単純化\ - ユーザランドでのプロトコル拡張が自由],
  [- コンテキストスイッチとパケット解析のコスト増大\ - リアルタイム組込み用途には重い]
)

=== 今後の設計検討課題・未決事項
+ *多 ISA 展開時の引数渡し規則*: ARM (Cortex-M), RISC-V, Xtensa 間でレジスタ引数スロット数 (a0..a3 / a0..a7) の差異をどう標準化するか。
+ *可変長ストリームの統合形態*: variable-length stream (ユーザー指定の設計方向) と object method-call の関係 (引数として stream handle を渡すのか、メソッド呼出し自体が stream 上に乗るのか)。
+ *非特権 origin の昇格防止*: 非特権オブジェクトが特権オブジェクトのメソッドを呼んだ際の、MPU リージョン再設定と実行権限昇格防止の厳密な条件。
+ *静的属性における authority の扱い*: 発注元指定の 4 属性と現行 `kernel_v3.typ` の 5 属性表記の整合性確定。

== 証跡・参照資料一覧

=== 一次資料・リポジトリリンク
- *Scarlet Repository*: #link("https://github.com/petitstrawberry/Scarlet/tree/0842752a2304225ab7b78a37757c9575a94195ae")[petitstrawberry/Scarlet (commit 0842752)]
  - カーネル syscall 実装: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/syscall/mod.rs#L295-L305")[kernel/src/syscall/mod.rs:295–305]
  - Handle 制御ディスパッチ: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/handle/syscall.rs#L203-L223")[kernel/src/object/handle/syscall.rs:203–223], #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/handle/syscall.rs#L250-L289")[250–289]
  - KernelObject 定義: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/mod.rs#L32-L53")[kernel/src/object/mod.rs:32–53]
  - ControlOps capability: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/kernel/src/object/capability/control.rs#L14-L33")[kernel/src/object/capability/control.rs:14–33]
  - ユーザランド Handle 抽象: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L217-L278")[user/lib/scarlet-os/src/handle/mod.rs:217–278], #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L281-L315")[281–315], #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/scarlet-os/src/handle/mod.rs#L332-L340")[332–340]
  - sbus IPC ライブラリ: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/lib.rs#L1-L32")[user/lib/sbus/src/lib.rs:1–32], #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/message.rs#L57-L72")[message.rs:57–72], #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus/src/types.rs#L7-L14")[types.rs:7–14]
  - sbus クライアント: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/user/lib/sbus-client/src/lib.rs#L181-L199")[user/lib/sbus-client/src/lib.rs:181–199]
  - ネイティブ API 契約文書: #link("https://github.com/petitstrawberry/Scarlet/blob/0842752a2304225ab7b78a37757c9575a94195ae/docs/release/1.0-native-api-contract.md#L18-L26")[docs/release/1.0-native-api-contract.md:18–26]
- *seL4*:
  - seL4 Reference Manual: #link("https://sel4.systems/Info/Docs/seL4-manual-latest.pdf#page=31")[seL4-manual-latest.pdf]
  - seL4 IPC Tutorial: #link("https://docs.sel4.systems/Tutorials/ipc.html")[docs.sel4.systems/Tutorials/ipc.html]
- *Fuchsia / Zircon*:
  - Zircon System Calls (`zx_channel_call`): #link("https://fuchsia.dev/reference/syscalls/channel_call")[fuchsia.dev/reference/syscalls/channel_call]
  - FIDL Overview: #link("https://fuchsia.dev/development/languages/fidl/overview")[fuchsia.dev/development/languages/fidl/overview]
- *Genode OS Framework*:
  - Genode OS Foundations Manual: #link("https://genode.org/documentation/genode-foundations-24-05.pdf#page=125")[genode-foundations-24-05.pdf]
  - Client-Server Tutorial (GENODE_RPC): #link("https://genode.org/documentation/developer-resources/client_server_tutorial")[client_server_tutorial]
- *Hubris*:
  - Oxide Computer Company Hubris Repository: #link("https://github.com/oxidecomputer/hubris")[github.com/oxidecomputer/hubris]
  - Hubris Reference Manual: #link("https://github.com/oxidecomputer/hubris/blob/master/doc/reference.adoc")[hubris/doc/reference.adoc]
- *Redox OS*:
  - The Redox OS Book Schemes: #link("https://doc.redox-os.org/book/ch04-03-schemes.html")[doc.redox-os.org/book/ch04-03-schemes.html]
  - Redox syscall repository: #link("https://gitlab.redox-os.org/redox-os/syscall")[gitlab.redox-os.org/redox-os/syscall]
- *L4Re / Fiasco.OC*:
  - L4Re IPC API: #link("https://l4re.org/doc/group__l4__ipc__api.html")[l4re.org/doc (L4 IPC API)]
  - L4Re C++ Interface: #link("https://l4re.org/doc/group__l4__cxx__ipc__ifaces.html")[l4re.org/doc (L4 C++ IPC Interfaces)]
- *QNX Neutrino RTOS*:
  - QNX Neutrino Architecture IPC: #link("https://www.blackberry.com/us/en/products/qnx-software-development-platform/qnx-neutrino-rtos")[QNX Neutrino RTOS Architecture]
  - QNX Message Copying: #link("https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/ipc_Message_copying.html")[qnx.com/.../ipc_Message_copying.html]

=== Shizuku 内部参照箇所
- `source/kernel_object/handler.cpp`: L318–325 (`current_object`), L327–335 (`caller_object`), L337–355 (`interposed_handler`), L533–581 (`call_method`), L620–648 (`forward_child_exit`), L1572–1690 (`handle`)
- `internal_headers/shizuku/object_api.hpp`: L15–139, L145–154, L190–215
- `docs/design/kernel_v3.typ`: L20, L34–37, L76–80, L89–100
- `docs/record/kernel_v2_record.typ`: 存在する (この調査の時点では不在だった。代替として読んだ 01、02 は削除済みで、要約は `docs/record/legacy_docs_summary.typ`)
