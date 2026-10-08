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

def config_substitutions(arch, board):
    """config_template.hpp.in の置換表 (arch / board だけがチップで変わる)。"""
    if arch == "rv32_c6":
        return _CONFIG_COMMON | {
            "${SHIZUKU_CPU_COUNT}": "1",
            "${SHIZUKU_MEMORY_MANAGER}": "shizuku::memory_managers::freestanding",
            "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
                "#include <shizuku/archs/%s.hpp>" % arch,
                "#include <shizuku/boards/%s.hpp>" % board,
                "#include <shizuku/memory_managers/freestanding.hpp>",
                "#include <shizuku/app_entry.hpp>",
            ]),
            "${SHIZUKU_ARCH}": arch,
            "${SHIZUKU_BOARD}": board,
        }
    return _CONFIG_COMMON | {
        "${INCLUDE_HEADERS_INSTRUCTION}": "\n".join([
            "#include <shizuku/archs/%s.hpp>" % arch,
            "#include <shizuku/boards/%s.hpp>" % board,
            "#include <shizuku/memory_managers/pico_sdk.hpp>",
            "#include <shizuku/app_entry.hpp>",
        ]),
        "${SHIZUKU_ARCH}": arch,
        "${SHIZUKU_BOARD}": board,
    }
