#ifndef SHIZUKU_DIAG_FP_HPP
#define SHIZUKU_DIAG_FP_HPP

#if defined(SHIZUKU_DIAG_FP)

#include <cstddef>
#include <cstdint>

namespace shizuku::diag_fp {

enum class point : uint8_t {
  K1 = 0,         // CALL SVC 入口最初 (shizuku_armv8m_svc_handler)
  K2 = 1,         // CALL 例外復帰直前 (return_resumed 後)
  K3 = 2,         // RETURN SVC 入口最初 (shizuku_armv8m_svc_handler)
  K3B_BEFORE = 3, // FloatingPointState::materialize() 直前
  K3B_AFTER = 4,  // FloatingPointState::materialize() 直後
  K3C_BEFORE = 5, // memmove 直前
  K3C_AFTER = 6,  // memmove 直後
  K4 = 7,         // RETURN caller 例外復帰直前 (return_resumed 後)
  COUNT = 8,
};

struct state_t {
  volatile uint32_t timestamps[static_cast<size_t>(point::COUNT)];
  uint32_t last_fpccr;
  uint8_t last_lspact[static_cast<size_t>(point::COUNT)];
  uint8_t last_control_fpca[static_cast<size_t>(point::COUNT)];

  // 累積差分 (64-bit でオーバーフロー防止)
  uint64_t accum_k2_k1;         // CALL ハンドラ (K2 - K1)
  uint64_t accum_k3_k2;         // Callee U + 例外復帰/エントリ (K3 - K2)
  uint64_t accum_k4_k3;         // RETURN ハンドラ全体 (K4 - K3)
  uint64_t accum_k3_k3b;        // RETURN 入口から materialize 直前
  uint64_t accum_k3b_mat;       // materialize (K3b_after - K3b_before)
  uint64_t accum_k3b_k3c;       // materialize 直後から memmove 直前
  uint64_t accum_k3c_memmove;   // memmove (K3c_after - K3c_before)
  uint64_t accum_k3c_k4;        // memmove 直後から K4
  uint64_t accum_caller_u;      // Caller U + 例外復帰/エントリ (K1_next - K4)

  uint32_t sample_count;        // K4 commit 回数
  uint32_t caller_u_count;      // K1_next - K4 サンプル数
  bool active;                  // 計測中フラグ
  bool has_k3b_k3c;             // 今回のラウンドで K3b/K3c を通過したか
  bool has_prev_k4;             // 前回の K4 タイムスタンプが有効か
  bool order_valid;             // 単調増加チェック結果
};

inline state_t g_state{};

// レジスタアドレス
inline volatile uint32_t *const REG_DWT_CYCCNT = reinterpret_cast<volatile uint32_t *>(0xE0001004);
inline volatile uint32_t *const REG_FPCCR      = reinterpret_cast<volatile uint32_t *>(0xE000EF34);

[[gnu::always_inline]] inline uint32_t read_cyccnt() {
#if defined(__ARM_ARCH_8M_MAIN__)
  return *REG_DWT_CYCCNT;
#else
  return 0;
#endif
}

[[gnu::always_inline]] inline uint32_t read_control_fpca() {
#if defined(__ARM_ARCH_8M_MAIN__)
  uint32_t ctrl;
  asm volatile("mrs %0, control" : "=r"(ctrl));
  return (ctrl >> 2) & 1u;
#else
  return 0;
#endif
}

[[gnu::always_inline]] inline uint32_t read_fpccr() {
#if defined(__ARM_ARCH_8M_MAIN__)
  return *REG_FPCCR;
#else
  return 0;
#endif
}

// 記録関数 (ホットパス)
[[gnu::always_inline]] inline void record_timestamp(point pt, uint32_t ts) {
  const size_t idx = static_cast<size_t>(pt);
  g_state.timestamps[idx] = ts;
#if defined(__ARM_ARCH_8M_MAIN__)
  const uint32_t fpccr = *REG_FPCCR;
  g_state.last_fpccr = fpccr;
  g_state.last_lspact[idx] = static_cast<uint8_t>(fpccr & 1u);
  g_state.last_control_fpca[idx] = static_cast<uint8_t>(read_control_fpca());
#else
  (void)ts;
#endif
}

[[gnu::always_inline]] inline void record_point(point pt) {
  record_timestamp(pt, read_cyccnt());
}

[[gnu::always_inline]] inline void on_k1(uint32_t entry_ts) {
  record_timestamp(point::K1, entry_ts);
  if (g_state.active && g_state.has_prev_k4) {
    const uint32_t prev_k4 = g_state.timestamps[static_cast<size_t>(point::K4)];
    const uint32_t d_caller = entry_ts - prev_k4;
    if (d_caller > 0 && d_caller < 0x80000000u) {
      g_state.accum_caller_u += d_caller;
      ++g_state.caller_u_count;
    }
  }
}

[[gnu::always_inline]] inline void on_k3(uint32_t entry_ts) {
  record_timestamp(point::K3, entry_ts);
}

[[gnu::always_inline]] inline void reset() {
  for (size_t i = 0; i < static_cast<size_t>(point::COUNT); ++i) {
    g_state.timestamps[i] = 0;
    g_state.last_lspact[i] = 0;
    g_state.last_control_fpca[i] = 0;
  }
  g_state.accum_k2_k1 = 0;
  g_state.accum_k3_k2 = 0;
  g_state.accum_k4_k3 = 0;
  g_state.accum_k3_k3b = 0;
  g_state.accum_k3b_mat = 0;
  g_state.accum_k3b_k3c = 0;
  g_state.accum_k3c_memmove = 0;
  g_state.accum_k3c_k4 = 0;
  g_state.accum_caller_u = 0;
  g_state.sample_count = 0;
  g_state.caller_u_count = 0;
  g_state.has_k3b_k3c = false;
  g_state.has_prev_k4 = false;
  g_state.order_valid = true;
  g_state.active = true;
}

[[gnu::always_inline]] inline void stop() {
  g_state.active = false;
  g_state.has_prev_k4 = false;
}

[[gnu::always_inline]] inline void commit_round() {
  if (!g_state.active) return;

  const uint32_t k1 = g_state.timestamps[static_cast<size_t>(point::K1)];
  const uint32_t k2 = g_state.timestamps[static_cast<size_t>(point::K2)];
  const uint32_t k3 = g_state.timestamps[static_cast<size_t>(point::K3)];
  const uint32_t k4 = g_state.timestamps[static_cast<size_t>(point::K4)];

  const uint32_t d_k2_k1 = k2 - k1;
  const uint32_t d_k3_k2 = k3 - k2;
  const uint32_t d_k4_k3 = k4 - k3;

  // 単調増加チェック (異常値判定: 各区間が 0 でなく、2^31 未満か)
  if (d_k2_k1 == 0 || d_k2_k1 >= 0x80000000u ||
      d_k3_k2 == 0 || d_k3_k2 >= 0x80000000u ||
      d_k4_k3 == 0 || d_k4_k3 >= 0x80000000u) {
    g_state.order_valid = false;
  }

  g_state.accum_k2_k1 += d_k2_k1;
  g_state.accum_k3_k2 += d_k3_k2;
  g_state.accum_k4_k3 += d_k4_k3;

  if (g_state.has_k3b_k3c) {
    const uint32_t k3b_b = g_state.timestamps[static_cast<size_t>(point::K3B_BEFORE)];
    const uint32_t k3b_a = g_state.timestamps[static_cast<size_t>(point::K3B_AFTER)];
    const uint32_t k3c_b = g_state.timestamps[static_cast<size_t>(point::K3C_BEFORE)];
    const uint32_t k3c_a = g_state.timestamps[static_cast<size_t>(point::K3C_AFTER)];

    g_state.accum_k3_k3b      += (k3b_b - k3);
    g_state.accum_k3b_mat     += (k3b_a - k3b_b);
    g_state.accum_k3b_k3c     += (k3c_b - k3b_a);
    g_state.accum_k3c_memmove += (k3c_a - k3c_b);
    g_state.accum_k3c_k4      += (k4 - k3c_a);
    g_state.has_k3b_k3c = false;
  }

  g_state.has_prev_k4 = true;
  ++g_state.sample_count;
}

} // namespace shizuku::diag_fp

#endif // defined(SHIZUKU_DIAG_FP)
#endif // SHIZUKU_DIAG_FP_HPP
