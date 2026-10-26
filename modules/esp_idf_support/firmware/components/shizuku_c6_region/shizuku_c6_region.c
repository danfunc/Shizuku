/*
 * 段1 experiment (ESP32-C6): IDF の esp_cpu_configure_region_protection を --wrap で置き換える。
 * ★SHIZUKU_EXP_C6_STAGE1=on のビルドだけ含まれる (firmware/CMakeLists.txt)。既定ビルドには入らない。
 *
 * 方針:
 *  - PMA は TRM v1.2 §1.9.5 p.69 の bit map: A[31:30] (TOR=01)、LOCK[29]、R[4]、W[3]、X[2]、TYPE[0] (1=valid)。
 *    PMA の R/W/X は特権モードに依らず適用される (TRM §1.9.3 p.67)。
 *  - PMA entry 0-12: IDF v6.1 の invalid 定数 (bit-exact)。bootloader が書き、app は読み直しで検証する (書込み後は LOCK)。
 *  - PMA entry 13-15 (app のみ): 13 = OFF+LOCK (addr13 = SOC_IRAM_LOW)、14 = TOR [SOC_IRAM_LOW, _iram_text_end) R|X、
 *    15 = TOR [_iram_text_end, SOC_DRAM_HIGH) R|W (X 無し)。_iram_text_end (linker) は 8B の .iram0.data の直前。
 *  - PMP には一切書かない (Shizuku が apply() で所有する。全 entry は unlock のまま)。
 *  - 失敗は fail-closed: 停止する (U-mode に入る前)。成功扱いのフォールバックは持たない。
 *
 * 出典: IDF v6.1 cpu_region_protect.c (公式、controller 確認: 行 28-61 invalid 定数、30-33 PMA 定数、105-114 PMA を PMP より先)、
 *        r6 ELF 逆アセンブル (esp_cpu_configure_invalid_regions 0x4200bf2a) で定数を再確認。
 */
#include <stdint.h>

#if !defined(BOOTLOADER_BUILD)
/* app 専用: PMA 0-15 を exact に読み直した後にだけ READY を書く (trap.cpp の U 経路が要求する)。
 * 0 = 未設定。.bss (static 記憶域) なので起動時に 0 になる想定。bootloader ビルドは参照も定義もしない。
 * 値は pma_c6::READY_MAGIC (chips/esp32_c6/pmp_c6.hpp) と一致させる。 */
#define SHIZUKU_C6_STAGE1_PMA_READY_MAGIC 0x5A1F0A11u
volatile uint32_t shizuku_c6_stage1_pma_ready = 0;
#endif

#define SHIZUKU_C6_SOC_IRAM_LOW   0x40800000u  /* HP_MEM 下端 (TRM §5.3 memory map) */
#define SHIZUKU_C6_SOC_DRAM_HIGH  0x40880000u  /* HP_MEM 上端 */

#define SHIZUKU_C6_CFG_A_TOR  0x40000000u
#define SHIZUKU_C6_CFG_LOCK   0x20000000u
#define SHIZUKU_C6_CFG_R      0x00000010u
#define SHIZUKU_C6_CFG_W      0x00000008u
#define SHIZUKU_C6_CFG_X      0x00000004u
#define SHIZUKU_C6_CFG_VALID  0x00000001u

/* IDF v6.1 invalid 定数 (entry 0-12)。偶数 entry は TOR deny、奇数は OFF+LOCK。 */
static const uint32_t shizuku_c6_idf_addr[13] = {
    0x08000000u, 0x0C000000u, 0x10000000u, 0x10014000u, 0x10200000u, 0x10220000u, 0x10800000u,
    0x10A00000u, 0x14000000u, 0x14001000u, 0x18000000u, 0x18040000u, 0x3FFFFFFFu};
static const uint32_t shizuku_c6_idf_cfg[13] = {
    0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u,
    0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u, 0x20000001u, 0x60000001u};

/* 失敗は停止 (fail-closed)。U-mode はここに到達する前に止まる。 */
static void shizuku_c6_region_fail(void) {
    for (;;) {
    }
}

/* PMA CSR アクセス (cfg 0xBC0-0xBCF、addr 0xBD0-0xBDF)。 */
static uint32_t shizuku_c6_pma_rd_cfg(unsigned i) {
    uint32_t v = 0;
    switch (i) {
    case 0: __asm__ volatile("csrr %0, 0xBC0" : "=r"(v)); break;
    case 1: __asm__ volatile("csrr %0, 0xBC1" : "=r"(v)); break;
    case 2: __asm__ volatile("csrr %0, 0xBC2" : "=r"(v)); break;
    case 3: __asm__ volatile("csrr %0, 0xBC3" : "=r"(v)); break;
    case 4: __asm__ volatile("csrr %0, 0xBC4" : "=r"(v)); break;
    case 5: __asm__ volatile("csrr %0, 0xBC5" : "=r"(v)); break;
    case 6: __asm__ volatile("csrr %0, 0xBC6" : "=r"(v)); break;
    case 7: __asm__ volatile("csrr %0, 0xBC7" : "=r"(v)); break;
    case 8: __asm__ volatile("csrr %0, 0xBC8" : "=r"(v)); break;
    case 9: __asm__ volatile("csrr %0, 0xBC9" : "=r"(v)); break;
    case 10: __asm__ volatile("csrr %0, 0xBCA" : "=r"(v)); break;
    case 11: __asm__ volatile("csrr %0, 0xBCB" : "=r"(v)); break;
    case 12: __asm__ volatile("csrr %0, 0xBCC" : "=r"(v)); break;
    case 13: __asm__ volatile("csrr %0, 0xBCD" : "=r"(v)); break;
    case 14: __asm__ volatile("csrr %0, 0xBCE" : "=r"(v)); break;
    case 15: __asm__ volatile("csrr %0, 0xBCF" : "=r"(v)); break;
    default: break;
    }
    return v;
}
static uint32_t shizuku_c6_pma_rd_addr(unsigned i) {
    uint32_t v = 0;
    switch (i) {
    case 0: __asm__ volatile("csrr %0, 0xBD0" : "=r"(v)); break;
    case 1: __asm__ volatile("csrr %0, 0xBD1" : "=r"(v)); break;
    case 2: __asm__ volatile("csrr %0, 0xBD2" : "=r"(v)); break;
    case 3: __asm__ volatile("csrr %0, 0xBD3" : "=r"(v)); break;
    case 4: __asm__ volatile("csrr %0, 0xBD4" : "=r"(v)); break;
    case 5: __asm__ volatile("csrr %0, 0xBD5" : "=r"(v)); break;
    case 6: __asm__ volatile("csrr %0, 0xBD6" : "=r"(v)); break;
    case 7: __asm__ volatile("csrr %0, 0xBD7" : "=r"(v)); break;
    case 8: __asm__ volatile("csrr %0, 0xBD8" : "=r"(v)); break;
    case 9: __asm__ volatile("csrr %0, 0xBD9" : "=r"(v)); break;
    case 10: __asm__ volatile("csrr %0, 0xBDA" : "=r"(v)); break;
    case 11: __asm__ volatile("csrr %0, 0xBDB" : "=r"(v)); break;
    case 12: __asm__ volatile("csrr %0, 0xBDC" : "=r"(v)); break;
    case 13: __asm__ volatile("csrr %0, 0xBDD" : "=r"(v)); break;
    case 14: __asm__ volatile("csrr %0, 0xBDE" : "=r"(v)); break;
    case 15: __asm__ volatile("csrr %0, 0xBDF" : "=r"(v)); break;
    default: break;
    }
    return v;
}
static void shizuku_c6_pma_wr_cfg(unsigned i, uint32_t v) {
    switch (i) {
    case 0: __asm__ volatile("csrw 0xBC0, %0" :: "r"(v) : "memory"); break;
    case 1: __asm__ volatile("csrw 0xBC1, %0" :: "r"(v) : "memory"); break;
    case 2: __asm__ volatile("csrw 0xBC2, %0" :: "r"(v) : "memory"); break;
    case 3: __asm__ volatile("csrw 0xBC3, %0" :: "r"(v) : "memory"); break;
    case 4: __asm__ volatile("csrw 0xBC4, %0" :: "r"(v) : "memory"); break;
    case 5: __asm__ volatile("csrw 0xBC5, %0" :: "r"(v) : "memory"); break;
    case 6: __asm__ volatile("csrw 0xBC6, %0" :: "r"(v) : "memory"); break;
    case 7: __asm__ volatile("csrw 0xBC7, %0" :: "r"(v) : "memory"); break;
    case 8: __asm__ volatile("csrw 0xBC8, %0" :: "r"(v) : "memory"); break;
    case 9: __asm__ volatile("csrw 0xBC9, %0" :: "r"(v) : "memory"); break;
    case 10: __asm__ volatile("csrw 0xBCA, %0" :: "r"(v) : "memory"); break;
    case 11: __asm__ volatile("csrw 0xBCB, %0" :: "r"(v) : "memory"); break;
    case 12: __asm__ volatile("csrw 0xBCC, %0" :: "r"(v) : "memory"); break;
    case 13: __asm__ volatile("csrw 0xBCD, %0" :: "r"(v) : "memory"); break;
    case 14: __asm__ volatile("csrw 0xBCE, %0" :: "r"(v) : "memory"); break;
    case 15: __asm__ volatile("csrw 0xBCF, %0" :: "r"(v) : "memory"); break;
    default: break;
    }
}
static void shizuku_c6_pma_wr_addr(unsigned i, uint32_t v) {
    switch (i) {
    case 0: __asm__ volatile("csrw 0xBD0, %0" :: "r"(v) : "memory"); break;
    case 1: __asm__ volatile("csrw 0xBD1, %0" :: "r"(v) : "memory"); break;
    case 2: __asm__ volatile("csrw 0xBD2, %0" :: "r"(v) : "memory"); break;
    case 3: __asm__ volatile("csrw 0xBD3, %0" :: "r"(v) : "memory"); break;
    case 4: __asm__ volatile("csrw 0xBD4, %0" :: "r"(v) : "memory"); break;
    case 5: __asm__ volatile("csrw 0xBD5, %0" :: "r"(v) : "memory"); break;
    case 6: __asm__ volatile("csrw 0xBD6, %0" :: "r"(v) : "memory"); break;
    case 7: __asm__ volatile("csrw 0xBD7, %0" :: "r"(v) : "memory"); break;
    case 8: __asm__ volatile("csrw 0xBD8, %0" :: "r"(v) : "memory"); break;
    case 9: __asm__ volatile("csrw 0xBD9, %0" :: "r"(v) : "memory"); break;
    case 10: __asm__ volatile("csrw 0xBDA, %0" :: "r"(v) : "memory"); break;
    case 11: __asm__ volatile("csrw 0xBDB, %0" :: "r"(v) : "memory"); break;
    case 12: __asm__ volatile("csrw 0xBDC, %0" :: "r"(v) : "memory"); break;
    case 13: __asm__ volatile("csrw 0xBDD, %0" :: "r"(v) : "memory"); break;
    case 14: __asm__ volatile("csrw 0xBDE, %0" :: "r"(v) : "memory"); break;
    case 15: __asm__ volatile("csrw 0xBDF, %0" :: "r"(v) : "memory"); break;
    default: break;
    }
}

/* 読み直し: entry 0-12 は IDF 定数、13-15 は exp_cfg/exp_addr と完全一致。不一致は停止。 */
static void shizuku_c6_pma_check(const uint32_t exp_cfg[16], const uint32_t exp_addr[16]) {
    for (unsigned i = 0; i < 16; ++i) {
        if (shizuku_c6_pma_rd_cfg(i) != exp_cfg[i] || shizuku_c6_pma_rd_addr(i) != exp_addr[i]) {
            shizuku_c6_region_fail();
        }
    }
}

#if defined(BOOTLOADER_BUILD)
/* bootloader: PMA 0-12 を IDF と同じ順 (addr, cfg の組) で書き、13-15 は OFF (0) のまま。PMP は触らない。
 * ★bootloader ビルドは linker の IRAM 終端シンボルを一切参照しない (未定義シンボルを残さないため、この節は app 節と排他)。 */
static void shizuku_c6_bootloader_branch(void) {
    uint32_t exp_cfg[16], exp_addr[16];
    for (unsigned i = 0; i < 13; ++i) {
        exp_cfg[i] = shizuku_c6_idf_cfg[i];
        exp_addr[i] = shizuku_c6_idf_addr[i];
        shizuku_c6_pma_wr_addr(i, exp_addr[i]);
        shizuku_c6_pma_wr_cfg(i, exp_cfg[i]);
    }
    for (unsigned i = 13; i < 16; ++i) {
        exp_cfg[i] = 0;
        exp_addr[i] = 0;
        shizuku_c6_pma_wr_cfg(i, 0);
        shizuku_c6_pma_wr_addr(i, 0);
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
    shizuku_c6_pma_check(exp_cfg, exp_addr);
}
#endif /* BOOTLOADER_BUILD */

#if !defined(BOOTLOADER_BUILD)
/* linker が定義する IRAM text の終端 (r6 map: 0x40809788)。app link だけがこのシンボルを必要とする。
 * bootloader link には未定義シンボルを残さない (app 節の中だけで宣言・参照する)。 */
extern char _iram_text_end[];

/* app: 0-12 は bootloader が LOCK 済みなので検証のみ。13-15 を設定してから読み直す。 */
static void shizuku_c6_app_branch(void) {
    const uint32_t low = SHIZUKU_C6_SOC_IRAM_LOW;
    const uint32_t text_end = (uint32_t)(uintptr_t)_iram_text_end;
    const uint32_t high = SHIZUKU_C6_SOC_DRAM_HIGH;
    /* 境界: 4 バイト整列、low < text_end < high、HP_MEM 内 (ここで外れたら停止) */
    if ((low & 3u) || (text_end & 3u) || (high & 3u)) shizuku_c6_region_fail();
    if (!(low < text_end && text_end < high)) shizuku_c6_region_fail();

    uint32_t exp_cfg[16], exp_addr[16];
    for (unsigned i = 0; i < 13; ++i) {
        exp_cfg[i] = shizuku_c6_idf_cfg[i];
        exp_addr[i] = shizuku_c6_idf_addr[i];
    }
    exp_cfg[13] = SHIZUKU_C6_CFG_LOCK;                                                   /* OFF + LOCK */
    exp_addr[13] = low >> 2;
    exp_cfg[14] = SHIZUKU_C6_CFG_A_TOR | SHIZUKU_C6_CFG_LOCK | SHIZUKU_C6_CFG_R | SHIZUKU_C6_CFG_X | SHIZUKU_C6_CFG_VALID;
    exp_addr[14] = text_end >> 2;
    exp_cfg[15] = SHIZUKU_C6_CFG_A_TOR | SHIZUKU_C6_CFG_LOCK | SHIZUKU_C6_CFG_R | SHIZUKU_C6_CFG_W | SHIZUKU_C6_CFG_VALID;
    exp_addr[15] = high >> 2;

    /* 事前: 0-12 は IDF 定数と一致、13-15 は cfg=0 かつ unlock (それ以外は停止) */
    for (unsigned i = 0; i < 13; ++i) {
        if (shizuku_c6_pma_rd_cfg(i) != shizuku_c6_idf_cfg[i] || shizuku_c6_pma_rd_addr(i) != shizuku_c6_idf_addr[i]) {
            shizuku_c6_region_fail();
        }
    }
    for (unsigned i = 13; i < 16; ++i) {
        const uint32_t c = shizuku_c6_pma_rd_cfg(i);
        if (c != 0u) shizuku_c6_region_fail();
    }
    /* 書込み順: addr13 -> cfg13 -> addr14 -> cfg14 -> addr15 -> cfg15 (TOR の LOCK が一つ下の addr を固定しうるため) */
    shizuku_c6_pma_wr_addr(13, exp_addr[13]);
    shizuku_c6_pma_wr_cfg(13, exp_cfg[13]);
    shizuku_c6_pma_wr_addr(14, exp_addr[14]);
    shizuku_c6_pma_wr_cfg(14, exp_cfg[14]);
    shizuku_c6_pma_wr_addr(15, exp_addr[15]);
    shizuku_c6_pma_wr_cfg(15, exp_cfg[15]);
    __asm__ volatile("fence rw,rw" ::: "memory");
    shizuku_c6_pma_check(exp_cfg, exp_addr); /* 不一致なら停止する。ここを通った = 16 entry が exact に一致 */
#if !defined(BOOTLOADER_BUILD)
    __asm__ volatile("fence rw,rw" ::: "memory");
    shizuku_c6_stage1_pma_ready = SHIZUKU_C6_STAGE1_PMA_READY_MAGIC; /* PMA ready: readback 後にだけ書く */
#endif
}
#endif /* !BOOTLOADER_BUILD */

/* IDF の呼出しを奪う。PMP は書かず、PMA 0-15 を設定 (bootloader/app で分岐) して読み直す。
 * 各ビルドは分岐を 1 つだけ含む (上の #if で排他)。 */
void __wrap_esp_cpu_configure_region_protection(void) {
#if defined(BOOTLOADER_BUILD)
    shizuku_c6_bootloader_branch();
#else
    shizuku_c6_app_branch();
#endif
}
