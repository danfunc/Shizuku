// Generic RV32 trap frame layout. Shared by preprocessed assembly and C++; offsets are bytes.
// チップ固有の値 (vector table、IRAM 配置など) は持たない。
#ifndef SHIZUKU_ARCH_RISCV_RV32_FRAME_H
#define SHIZUKU_ARCH_RISCV_RV32_FRAME_H
#define RV32_F_RA 0
#define RV32_F_T0 4
#define RV32_F_T1 8
#define RV32_F_T2 12
#define RV32_F_A0 16
#define RV32_F_A1 20
#define RV32_F_A2 24
#define RV32_F_A3 28
#define RV32_F_A4 32
#define RV32_F_A5 36
#define RV32_F_A6 40
#define RV32_F_A7 44
#define RV32_F_T3 48
#define RV32_F_T4 52
#define RV32_F_T5 56
#define RV32_F_T6 60
#define RV32_F_MEPC 64
#define RV32_F_MSTATUS 68
#define RV32_F_MCAUSE 72
#define RV32_F_MTVAL 76
#define RV32_F_S0 80
#define RV32_F_S1 84
#define RV32_F_S2 88
#define RV32_F_S3 92
#define RV32_F_S4 96
#define RV32_F_S5 100
#define RV32_F_S6 104
#define RV32_F_S7 108
#define RV32_F_S8 112
#define RV32_F_S9 116
#define RV32_F_S10 120
#define RV32_F_S11 124
#define RV32_F_GP 128
#define RV32_F_TP 132
#define RV32_F_RESERVED0 136
#define RV32_F_RESERVED1 140
#define RV32_FRAME_BYTES 144
// 呼出しの記録 (call_header_t) を置く枠。ecall の前に caller stub が sp をこれだけ余分に下げ、
// カーネルが枠 [sp, sp + RV32_CALL_HEADER_BYTES) に書く (design notes §5b)。
#define RV32_CALL_HEADER_BYTES 32
// フレームの mcause に載せる出口の目印。ECALL のフレームはこの値になる (本物の例外コードは 11 =
// M-mode ECALL と同じ値)。カーネルが呼び先の入口フレームを作ると RV32_TAG_ENTRY に書き換える。
// それ以外の値 (割込み・例外・スレッドの初期フレーム = 0) は全レジスタを復元して戻る。
//   RV32_TAG_ECALL       : ECALL の戻り (素通し)。mepc / mstatus / a0 / a1 / sp だけを戻し、ほかの
//                          レジスタはカーネルを抜けた時点の値のまま (消さない。隠したい値の消去は
//                          callee の責任、caller は a0 / a1 以外を信用しない)。
//   RV32_TAG_ECALL_SCRUB : callee が自分の意思で戻らない巻き戻し (多段の EXIT_METHOD / RETURN、
//                          FORWARD_CHILD_EXIT、別スレッドへ切り替わったあとの再開) の戻り。
//                          出口は ra・t*・a2-a7・s* を 0 に消す (gp / tp は触らない)。
//   RV32_TAG_ENTRY       : 呼び先の入口 (素通し)。ra と引数 a0-a7、pc、sp だけをカーネルが決める。
#define RV32_TAG_ECALL 11
#define RV32_TAG_ENTRY 0x100
#define RV32_TAG_ECALL_SCRUB 0x101
#define RV32_TAG_U_ENTRY 0x110
#define RV32_TAG_U_ECALL 0x111
#define RV32_TAG_U_SCRUB 0x112
// M-mode ECALL を a0 の番号で直接振り分ける表 (shizuku_rv32_ecall_table、chip の trap.cpp が持つ) の
// 要素数。object_api の番号 0..33 (svc_attributes の大きさ) と同じ。範囲外の番号は 0 番の要素
// (= thread mode の API・primitive・raw 配送を従来どおり決める共通の入口) に落とす。
#define RV32_ECALL_TABLE_ENTRIES 34
#define RV32_MSTATUS_MPP 0x1800
#define RV32_MSTATUS_MPIE 0x80
#endif
