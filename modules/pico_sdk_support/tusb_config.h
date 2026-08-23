// ★Shizuku 用の TinyUSB 設定。**CDC を 2 本**にするためだけに SDK の既定を置き換える。
//   1 本目 = 診断 (printf)、2 本目 = GDB。混ぜると握手が壊れる (D41/D42)。
#ifndef SHIZUKU_TUSB_CONFIG_H
#define SHIZUKU_TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUD_CDC (2)
#define CFG_TUD_CDC_RX_BUFSIZE (256)
#define CFG_TUD_CDC_TX_BUFSIZE (256)
#define CFG_TUD_CDC_EP_BUFSIZE (64)
// リセットインターフェースは自前のドライバで捌く (TinyUSB の vendor は使わない)。
#define CFG_TUD_VENDOR (0)

#endif // SHIZUKU_TUSB_CONFIG_H
