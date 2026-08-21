"""CYW43 の gSPI クロックを clk_sys から導出する (docs/03_porting_policy.md D23)。

★分周比を直書きしない。SDK の CYW43_PIO_CLOCK_DIV_INT は 2 固定で clk_sys から
算出しないので、clk_sys を上げると SPI も一緒に上がってチップが黙る (実測:
300MHz/div2 = SCK 75MHz で無応答)。逆に 4 を直書きすると 150MHz で半速になる。

★SCK = clk_sys / (div * 2)。2 が余分に見えるが、PIO のプログラムが 1 ビットに
2 命令使うため。参照実装の「SPI target 75MHz」はこの 2 を含まない SM クロック。

★★分周比とプログラムは**対で選ぶ**。SDK は PIO プログラムを固定で選んでおり、
2 つは応答をサンプルする位置が違う。位置は SM サイクル単位なので、分周比を変えると
サンプル点が実時間でずれる。実測 (2026-08-21): div=2 のまま SCK を 18.75MHz へ
落としたら Failed to read test pattern でチップが上がらなかった。
"""

# 今はクロックを設定していないので SDK 既定の値。上げるならここを変える。
SYS_CLOCK_KHZ = 150000
# 目標 SCK。上限は CYW43439 の gSPI 定格 (50MHz)。既知の実測点は 37.5MHz = 可 /
# 75MHz = 不可 の 2 点だけなので、間を上げるなら実測してから。
CYW43_SCK_KHZ = 37500

def _ceil_div(a, b):
    return (a + b - 1) // b

CYW43_PIO_DIV = max(1, _ceil_div(SYS_CLOCK_KHZ, 2 * CYW43_SCK_KHZ))
CYW43_SCK_ACTUAL_KHZ = SYS_CLOCK_KHZ // (2 * CYW43_PIO_DIV)

# div が大きい = 遅い側では、応答を早めにサンプルするプログラムを使う。
CYW43_SPI_PROGRAM = "spi_gap0_sample1" if CYW43_PIO_DIV > 2 else "spi_gap01_sample0"
