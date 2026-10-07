#ifndef SHIZUKU_FREESTANDING_MEM_HPP
#define SHIZUKU_FREESTANDING_MEM_HPP
#include <cstddef>
#include <cstdint>

// ===========================================================================
//  オブジェクト (非特権) 側で使ってよい mem 系 — ライブラリ関数を呼ばない版
// ===========================================================================
//  ★非特権のコードは libc / コンパイラ rt の memcpy・memset を呼んではいけない。
//    RP2040 の newlib/pico_mem_ops は bootrom の mem 関数 (0x0000xxxx、特権専用) へ
//    飛ぶので、非特権から呼ぶと MPU に落とされる。オブジェクトを将来「システム
//    コールだけで作る独立バイナリ」にするときも、リンクできる mem はここだけ。
//  ★インライン (always_inline) にして、呼び出し元の配置 (RAM / flash) に従わせる。
//  ★ループ内の空 asm は、GCC がこのループを memcpy/memset 呼出しへ書き戻す
//    (-ftree-loop-distribute-patterns) のを止めるためにある。消さないこと。
#define SHIZUKU_MEM_INLINE [[gnu::always_inline]] inline

namespace shizuku {

SHIZUKU_MEM_INLINE void shizuku_memcpy(void *dst, const void *src,
                                       size_t bytes) {
  uint8_t *d = static_cast<uint8_t *>(dst);
  const uint8_t *s = static_cast<const uint8_t *>(src);
  if ((((uintptr_t)d | (uintptr_t)s | bytes) & 3u) == 0) {
    using alias_word = uint32_t __attribute__((may_alias));
    alias_word *dw = reinterpret_cast<alias_word *>(d);
    const alias_word *sw = reinterpret_cast<const alias_word *>(s);
    size_t i = 0;
    // Four words per loop: -Og previously paid branch/index overhead per word.
    // may_alias preserves the byte-copy contract; barriers prevent libc folding.
    for (; i + 4 <= bytes / 4; i += 4) {
      const alias_word a = sw[i], b = sw[i + 1], c = sw[i + 2], e = sw[i + 3];
      dw[i] = a; dw[i + 1] = b; dw[i + 2] = c; dw[i + 3] = e;
      __asm__ volatile("" ::: "memory");
    }
    for (; i < bytes / 4; ++i) {
      dw[i] = sw[i];
      __asm__ volatile("" ::: "memory");
    }
    return;
  }
  for (size_t i = 0; i < bytes; ++i) {
    d[i] = s[i];
    __asm__ volatile("" ::: "memory");
  }
}

SHIZUKU_MEM_INLINE void shizuku_memset(void *dst, uint8_t value,
                                       size_t bytes) {
  uint8_t *d = static_cast<uint8_t *>(dst);
  for (size_t i = 0; i < bytes; ++i) {
    d[i] = value;
    __asm__ volatile("" ::: "memory");
  }
}

// 型付きの 1 レコード複写 (trivially copyable 前提)。
template <typename T>
SHIZUKU_MEM_INLINE void shizuku_copy_record(T *dst, const T *src) {
  shizuku_memcpy(dst, src, sizeof(T));
}

// 値初期化 `T x{}` の代わり (暗黙の memset を避ける)。
template <typename T> SHIZUKU_MEM_INLINE void shizuku_zero_record(T *dst) {
  shizuku_memset(dst, 0, sizeof(T));
}

} // namespace shizuku
#endif // SHIZUKU_FREESTANDING_MEM_HPP
