"""RP2040 Pico SDK Firmware Composition & CMake Linking Starlark API (configs/rp_firmware.bzl)

Provides:
- rp_firmware: Composition, full archive bundle (kernel + kernel_object + objects + metadata),
               and final CMake Pico SDK UF2 link for RP2040.
- rp_objects_archive: Bounded SDK-free objects-only archive for testing/integration.
- rp_sketch: Single-sketch RP2040 UF2 macro.
"""

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//:defs.bzl",
    "rp_firmware_binary",
    "shizuku_composition_gen",
    "shizuku_firmware",
    "shizuku_firmware_archive_bundle",
    "shizuku_sketch",
)

_rp_firmware_binary = rp_firmware_binary

# ==============================================================================
# Public RP2040 Macros: rp_objects_archive, rp_firmware, rp_sketch
# ==============================================================================
def rp_objects_archive(
        name,
        target = "rp2040",
        objects = [],
        parents = {},
        privileged_names = [],
        **kwargs):
    """SDK-free なオブジェクト群 + 合成マニフェスト + ID 定義のみを束ねるアーカイブ。"""
    if target == "rp2350":
        fail("Target 'rp2350' is unsupported for rp_objects_archive (RP2350 is a follow-up and must not be declared supported; only 'rp2040' is supported).")
    if target != "rp2040":
        fail("Unsupported target '%s' for rp_objects_archive. RP2040 is the only supported target." % target)

    gen_name = name + "_composition_gen"
    meta_name = name + "_composition_meta"

    shizuku_composition_gen(
        name = gen_name,
        objects = objects,
        target = target,
        parents = parents,
        privileged_names = privileged_names,
        tags = kwargs.get("tags", []),
    )

    cc_library(
        name = meta_name,
        srcs = [":" + gen_name],
        tags = kwargs.get("tags", []),
    )

    shizuku_firmware_archive_bundle(
        name = name,
        target = target,
        objects = objects,
        metadata = ":" + meta_name,
        tags = kwargs.get("tags", []),
        visibility = kwargs.get("visibility", ["//visibility:public"]),
    )

def rp_firmware(
        name,
        target = "rp2040",
        board = "pico",
        objects = [],
        parents = {},
        privileged_names = [],
        kernel = None,
        kernel_object = None,
        pico_sdk_path = "",
        build_dir = "",
        config_header = "@shizuku//configs:config_hpp",
        **kwargs):
    """RP2040 Pico SDK CMake 最終リンク UF2 を生成する完全ファームウェアマクロ。"""
    if target == "rp2350":
        fail("Target 'rp2350' is unsupported for rp_firmware (RP2350 is a follow-up and must not be declared supported; only 'rp2040' is supported).")
    if target != "rp2040":
        fail("Unsupported target '%s' for rp_firmware. RP2040 is the only supported target." % target)
    if board != "pico":
        fail("Unsupported board '%s' for rp_firmware. Only 'pico' is currently supported (pico_w is not yet supported for standalone composition)." % board)

    shizuku_firmware(
        name = name,
        target = target,
        board = board,
        objects = objects,
        parents = parents,
        privileged_names = privileged_names,
        kernel = kernel,
        kernel_object = kernel_object,
        pico_sdk_path = pico_sdk_path,
        build_dir = build_dir,
        config_header = config_header,
        **kwargs
    )

def rp_sketch(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        target = "rp2040",
        board = "pico",
        setup_fn = "setup",
        loop_fn = "loop",
        kernel = None,
        kernel_object = None,
        pico_sdk_path = "",
        build_dir = "",
        config_header = "@shizuku//configs:config_hpp",
        **kwargs):
    """単一スケッチから RP2040 Pico SDK UF2 を作成する糖衣マクロ。"""
    if target == "rp2350":
        fail("Target 'rp2350' is unsupported for rp_sketch (RP2350 is a follow-up and must not be declared supported; only 'rp2040' is supported).")
    if target != "rp2040":
        fail("Unsupported target '%s' for rp_sketch. RP2040 is the only supported target." % target)
    if board != "pico":
        fail("Unsupported board '%s' for rp_sketch. Only 'pico' is currently supported (pico_w is not yet supported for standalone composition)." % board)

    shizuku_sketch(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        target = target,
        board = board,
        setup_fn = setup_fn,
        loop_fn = loop_fn,
        kernel = kernel,
        kernel_object = kernel_object,
        pico_sdk_path = pico_sdk_path,
        build_dir = build_dir,
        config_header = config_header,
        **kwargs
    )
