"""ESP32-C6 (RV32IMAC) 用 cc toolchain 設定 (riscv32-esp-elf GCC esp-15.2.0_20251204)。

★ARM 側 (pico-sdk) のツールチェーンには触れない。この toolchain は
  @platforms//cpu:riscv32 + @platforms//os:none の target にだけ解決される。
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)

# IDF の esp32c6 と同じ ISA / ABI。違うと IDF 側のオブジェクトとリンクできない。
_ISA = ["-march=rv32imac_zicsr_zifencei", "-mabi=ilp32"]

_COMPILE_ACTIONS = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
]
_CXX_ACTIONS = [ACTION_NAMES.cpp_compile]
_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

# (Bazel の tool 名, toolchains/rv32_esp/bin/ のラッパ名)。ラッパは実体の GCC を exec する。
_TOOLS = [
    ("gcc", "bin/gcc"),
    ("g++", "bin/g++"),
    ("cpp", "bin/cpp"),
    ("ar", "bin/ar"),
    ("ld", "bin/ld"),
    ("nm", "bin/nm"),
    ("objdump", "bin/objdump"),
    ("strip", "bin/strip"),
    ("gcov", "bin/gcov"),
]

def _impl(ctx):
    tool_paths = [tool_path(name = name, path = path) for name, path in _TOOLS]

    default_flags = feature(
        name = "default_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = _ISA + [
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-fno-common",
                ])],
            ),
            # カーネルは例外・RTTI を使わない (IDF 側の C++ ランタイムに依存しない)。
            flag_set(
                actions = _CXX_ACTIONS,
                flag_groups = [flag_group(flags = ["-fno-exceptions", "-fno-rtti"])],
            ),
            flag_set(
                actions = _LINK_ACTIONS,
                flag_groups = [flag_group(flags = _ISA)],
            ),
        ],
    )

    # Bazel の汎用 feature。入れないと dbg/opt の組み込みが効かない。
    supports_pic = feature(name = "supports_pic", enabled = False)
    dbg = feature(name = "dbg")
    opt = feature(name = "opt")
    fastbuild = feature(name = "fastbuild")

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "rv32_esp_elf",
        host_system_name = "local",
        target_system_name = "riscv32-esp-elf",
        target_cpu = "riscv32",
        target_libc = "newlib",
        compiler = "gcc",
        abi_version = "ilp32",
        abi_libc_version = "newlib",
        tool_paths = tool_paths,
        features = [default_flags, supports_pic, dbg, opt, fastbuild],
        cxx_builtin_include_directories = ctx.attr.builtin_include_directories,
    )

rv32_esp_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = dict(builtin_include_directories = attr.string_list()),
    provides = [CcToolchainConfigInfo],
)
