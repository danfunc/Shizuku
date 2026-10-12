"""config.hpp の kit 変数 (configs/BUILD.bazel の config_hpp が使う)。"""

# kit 変数 (docs/03_porting_policy.md §1)。チップで変わるのは ARCH / BOARD と
# その include だけなので、それ以外は共通にしておく。
_CONFIG_COMMON = {
    "${SHIZUKU_CPU_COUNT}": "2",
    "${SHIZUKU_THREAD_COUNT}": "32",
    # ★flight_robocon_telemetory_sender (XNO 役) が別リポジトリから
    #   ble_uart/bno055/bme280 等を足すため 32 では埋まる (Shizuku 自身が
    #   30 使用 + 消費側 2〜3 個で即オーバー、実測: bme280 の CREATE_OBJECT
    #   が範囲外で失敗した)。余裕を持たせておく。
    "${SHIZUKU_OBJECT_COUNT}": "64",
    "${SHIZUKU_METHOD_COUNT}": "16",
    "${SHIZUKU_CALL_DEPTH}": "32",
    "${SHIZUKU_MEMORY_MANAGER}": "shizuku::memory_managers::pico_sdk",
}

def config_substitutions(arch, chip, board, ble = False):
    """config_template.hpp.in の置換表。ARCH / CHIP / BOARD の 3 軸を注入する。

    ARCH (ISA), CHIP (SoC の定義), BOARD (ボード実装) はそれぞれ別の軸で、
    呼び出し側 (configs/BUILD.bazel) が組み合わせを決める。
    """
    if arch == "rv32_c5":
        c5_config = _CONFIG_COMMON | {
            "${SHIZUKU_CPU_COUNT}": "1",
            "${SHIZUKU_MEMORY_MANAGER}": "shizuku::memory_managers::freestanding",
            "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
                '#include "chips/esp32_c5/chip.hpp"',
                '#include "chips/esp32_c5/rv32_c5.hpp"',
                '#include "boards/esp32_c5/esp32_c5.hpp"',
                '#include "shizuku/memory_managers/freestanding.hpp"',
                '#include "shizuku/app_entry.hpp"',
            ]),
        }
        if ble:
            # Controller-init profile needs far fewer concurrent Shizuku calls
            # than the general C5 selftest/application profile.
            c5_config = c5_config | {
                "${SHIZUKU_THREAD_COUNT}": "10",
                "${SHIZUKU_CALL_DEPTH}": "8",
            }
        return c5_config | {
            "${SHIZUKU_ARCH}": arch,
            "${SHIZUKU_ARCH_TYPE}": "shizuku::chips::esp32_c5::chip::arch_type",
            "${SHIZUKU_CHIP}": chip,
            "${SHIZUKU_BOARD}": board,
        }
    if arch == "rv32_c6":
        return _CONFIG_COMMON | {
            "${SHIZUKU_CPU_COUNT}": "1",
            "${SHIZUKU_MEMORY_MANAGER}": "shizuku::memory_managers::freestanding",
            "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
                '#include "chips/esp32_c6/chip.hpp"',
                '#include "chips/esp32_c6/rv32_c6.hpp"',
                '#include "boards/esp32_c6/esp32_c6.hpp"',
                '#include "shizuku/memory_managers/freestanding.hpp"',
                '#include "shizuku/app_entry.hpp"',
            ]),
            "${SHIZUKU_ARCH}": arch,
            "${SHIZUKU_ARCH_TYPE}": "shizuku::chips::esp32_c6::chip::arch_type",
            "${SHIZUKU_CHIP}": chip,
            "${SHIZUKU_BOARD}": board,
        }
    if arch == "armv6m":
        return _CONFIG_COMMON | {
            "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
                '#include "arch/armv6m/armv6m.hpp"',
                '#include "chips/rp2040/chip.hpp"',
                '#include "boards/rp2040_pico_w/rp2040_pico_w.hpp"',
                '#include "shizuku/memory_managers/pico_sdk.hpp"',
                '#include "shizuku/app_entry.hpp"',
            ]),
            "${SHIZUKU_ARCH}": arch,
            "${SHIZUKU_ARCH_TYPE}": "shizuku::archs::armv6m<shizuku::chips::rp2040::chip::kMpuRegionCount, shizuku::chips::rp2040::chip::kNvicPriorityBits>",
            "${SHIZUKU_CHIP}": chip,
            "${SHIZUKU_BOARD}": board,
        }
    return _CONFIG_COMMON | {
        "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
            '#include "arch/armv8m/armv8m.hpp"',
            '#include "chips/rp2350/chip.hpp"',
            '#include "boards/rp2350_pico2/rp2350_pico2.hpp"',
            '#include "shizuku/memory_managers/pico_sdk.hpp"',
            '#include "shizuku/app_entry.hpp"',
        ]),
        "${SHIZUKU_ARCH}": arch,
        "${SHIZUKU_ARCH_TYPE}": "shizuku::archs::armv8m<shizuku::chips::rp2350::chip::kMpuRegionCount, shizuku::chips::rp2350::chip::kNvicPriorityBits>",
        "${SHIZUKU_CHIP}": chip,
        "${SHIZUKU_BOARD}": board,
    }
