"""Shizuku Object & Firmware Composition API (defs.bzl)

提供ルール・マクロ:
- ShizukuObjectInfo: オブジェクトメタデータ provider (object_name, entry_point, cc_info)
- shizuku_object: firmware/ID/parent 未定の純粋 cc_library compile unit
- shizuku_sketch_object: setup()/loop() 入口ラッパーを持つ shizuku_object
- shizuku_firmware: 合成時 ID 採番・親子・権限・chip 決定、単一 .a および .bin 生成
- shizuku_sketch: 単一スケッチからファームウェアまで直行する糖衣マクロ
"""

load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# ==============================================================================
# 1. ShizukuObjectInfo Provider
# ==============================================================================
ShizukuObjectInfo = provider(
    doc = "Shizuku Object marker and metadata for firmware composition",
    fields = {
        "object_name": "Unique name of the object (string)",
        "entry_point": "Entry function symbol name (string, e.g. <name>_main)",
        "cc_info": "CcInfo provider containing compiled object files and include paths",
    },
)

# ==============================================================================
# 2. shizuku_object
# ==============================================================================
def _shizuku_object_marker_impl(ctx):
    cc_info = ctx.attr.dep[CcInfo]
    info = ShizukuObjectInfo(
        object_name = ctx.attr.object_name,
        entry_point = ctx.attr.entry_point,
        cc_info = cc_info,
    )
    return [
        info,
        cc_info,
        DefaultInfo(files = ctx.attr.dep[DefaultInfo].files),
    ]

_shizuku_object_marker = rule(
    implementation = _shizuku_object_marker_impl,
    attrs = {
        "object_name": attr.string(mandatory = True),
        "entry_point": attr.string(mandatory = True),
        "dep": attr.label(mandatory = True, providers = [CcInfo]),
    },
)

def shizuku_object(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        object_name = None,
        entry_point = None,
        **kwargs):
    """firmware/ID/parent/permission 未定の cc_library compile unit。

    同 chip/toolchain であれば合成依存なく再コンパイル不要。
    親オブジェクトや権限メタデータは firmware 合成側が所有する。
    """
    if "@shizuku//:internal_headers" in deps or "//:internal_headers" in deps:
        fail("shizuku_object cannot depend on legacy internal_headers (defines global IDs); use //:core_headers")

    impl_name = name + "_impl"
    actual_obj_name = object_name if object_name else name
    ep = entry_point if entry_point else (actual_obj_name + "_main")
    object_deps = deps if "@shizuku//:core_headers" in deps or "//:core_headers" in deps else deps + ["@shizuku//:core_headers"]

    cc_library(
        name = impl_name,
        srcs = srcs,
        hdrs = hdrs,
        deps = object_deps,
        **kwargs
    )

    _shizuku_object_marker(
        name = name,
        object_name = actual_obj_name,
        entry_point = ep,
        dep = ":" + impl_name,
        visibility = kwargs.get("visibility", ["//visibility:public"]),
        tags = kwargs.get("tags", []),
    )

# ==============================================================================
# 3. Composition Transition & Metadata Generator
# ==============================================================================
_PICO_BOARD_SETTING = str(Label("@pico-sdk//bazel/config:PICO_BOARD"))

def _firmware_transition_impl(settings, attr):
    target = attr.target
    if target == "esp32c6":
        return {
            "//command_line_option:platforms": str(Label("@shizuku//toolchains/rv32_esp:esp32c6")),
            str(Label("@shizuku//configs:board")): "esp32_c6",
            str(Label("@shizuku//configs:rp2040_cmake_profile")): False,
            _PICO_BOARD_SETTING: settings[_PICO_BOARD_SETTING],
            "//command_line_option:cxxopt": ["-std=c++23"],
            "//command_line_option:copt": ["-Og", "-g3", "-DSHIZUKU_C6_SKELETON_PERMITTED=1"],
            "//command_line_option:strip": "never",
        }
    elif target == "esp32c5":
        return {
            "//command_line_option:platforms": str(Label("@shizuku//toolchains/rv32_esp:esp32c6")),
            str(Label("@shizuku//configs:board")): "esp32_c5",
            str(Label("@shizuku//configs:rp2040_cmake_profile")): False,
            _PICO_BOARD_SETTING: settings[_PICO_BOARD_SETTING],
            "//command_line_option:cxxopt": ["-std=c++23"],
            "//command_line_option:copt": ["-Og", "-g3", "-DSHIZUKU_C5_SKELETON_PERMITTED=1"],
            "//command_line_option:strip": "never",
        }
    elif target == "rp2040":
        return {
            "//command_line_option:platforms": str(Label("@pico-sdk//bazel/platform:rp2040")),
            str(Label("@shizuku//configs:board")): "rp2040_pico",
            str(Label("@shizuku//configs:rp2040_cmake_profile")): True,
            _PICO_BOARD_SETTING: "pico",
            "//command_line_option:cxxopt": ["-std=c++23"],
            "//command_line_option:copt": ["-Og", "-g3", "-DSHIZUKU_RP2040=1"],
            "//command_line_option:strip": "never",
        }
    elif target == "rp2350":
        fail("Target 'rp2350' is unsupported (RP2350 support is a follow-up; only 'rp2040' is supported).")
    else:
        fail("Unknown target: '%s'. Supported targets are 'esp32c6', 'esp32c5', 'rp2040' ('rp2350' is unsupported)." % target)

_firmware_transition = transition(
    implementation = _firmware_transition_impl,
    inputs = [
        _PICO_BOARD_SETTING,
    ],
    outputs = [
        "//command_line_option:platforms",
        str(Label("@shizuku//configs:board")),
        str(Label("@shizuku//configs:rp2040_cmake_profile")),
        _PICO_BOARD_SETTING,
        "//command_line_option:cxxopt",
        "//command_line_option:copt",
        "//command_line_option:strip",
    ],
)

def _shizuku_composition_gen_impl(ctx):
    # 0. ターゲット検証 (Fail-closed)
    target = ctx.attr.target
    if target == "rp2350":
        fail("Target 'rp2350' is unsupported (RP2350 support is a follow-up; only 'rp2040' is supported).")
    if target not in ["esp32c6", "esp32c5", "rp2040"]:
        fail("Unknown target: '%s'. Supported targets are 'esp32c6', 'esp32c5', 'rp2040' ('rp2350' is unsupported)." % target)

    # 1. オブジェクト検証 (Fail-closed)
    objects = ctx.attr.objects
    names = []
    seen = {}

    for obj in objects:
        if ShizukuObjectInfo not in obj:
            fail("Target '%s' does not provide ShizukuObjectInfo" % obj.label)
        info = obj[ShizukuObjectInfo]
        name = info.object_name
        if name in seen:
            fail("Duplicate object name: '%s' (conflicting targets: %s, %s)" % (name, seen[name], obj.label))
        if name in ["root", "count", "kernel", "kernel_object"]:
            fail("Reserved object name: '%s' is not allowed" % name)
        seen[name] = obj.label
        names.append(name)

    # 予約スロット (0: KERNEL_OBJECT, 1: ROOT_OBJECT) を引いた上限 62
    if len(names) > 62:
        fail("Object count exceeds capacity limit (62): got %d" % len(names))

    # 2. 親子関係 (parents) と循環検証 (Fail-closed)
    parents_dict = ctx.attr.parents
    for child, parent in parents_dict.items():
        if child not in seen:
            fail("Cannot set parent for unknown child object '%s'" % child)
        if parent == child:
            fail("Self-parenting cycle detected for object '%s'" % child)
        if parent != "root" and parent not in seen:
            fail("Missing parent object '%s' for object '%s'" % (parent, child))

        # サイクル検出 (DFS)
        curr = parent
        visited = [child]
        for _ in range(len(names) + 2):
            if curr == "root" or curr not in parents_dict:
                break
            if curr in visited:
                fail("Cycle detected in parent hierarchy for object '%s' (cycle path: %s -> %s)" % (child, " -> ".join(visited), curr))
            visited.append(curr)
            curr = parents_dict[curr]

    # 3. Composition ID 定義ソースの生成 (tools/gen_object_ids.py 呼び出し)
    # canonical exported C ABI symbol を含む .c ファイルを出力
    ids_out = ctx.actions.declare_file(ctx.label.name + "_composition_ids.c")
    args = ctx.actions.args()
    args.add("--mode", "extern")
    args.add("--source-template", ctx.file.source_template)
    args.add("--source-out", ids_out)
    args.add("--limit", "64")
    if names:
        args.add("--names")
        args.add_all(names)

    ctx.actions.run(
        outputs = [ids_out],
        inputs = [ctx.file.source_template],
        executable = ctx.executable.gen_ids_tool,
        arguments = [args],
        mnemonic = "GenCompositionIds",
        progress_message = "Generating composition object IDs for %s" % ctx.label,
    )

    # 4. Manifest ソースの生成 (ランタイム初期入力テーブル)
    # ★不変条件: manifest root parent_id は runtime ROOT_OBJECT = 1。legacy symbol root = 0 とは分離。
    manifest_out = ctx.actions.declare_file(ctx.label.name + "_manifest.c")
    manifest_lines = [
        "// Generated composition manifest for " + ctx.label.name,
        "#include <stdint.h>",
        "#include <stdbool.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    uintptr_t id;",
        "    void (*entry)(void);",
        "    uintptr_t parent_id;",
        "    bool privileged;",
        "} shizuku_object_manifest_entry_t;",
        "",
    ]
    for obj in objects:
        info = obj[ShizukuObjectInfo]
        manifest_lines.append("extern void %s(void);" % info.entry_point)
    manifest_lines.append("")
    manifest_lines.append("const shizuku_object_manifest_entry_t shizuku_composition_manifest[] = {")

    # ID 割り当てマップ (name -> id): index 2 から
    id_map = {}
    curr_id = 2
    for name in names:
        id_map[name] = curr_id
        curr_id += 1

    privileged_set = {p: True for p in ctx.attr.privileged_names}

    for obj in objects:
        info = obj[ShizukuObjectInfo]
        obj_name = info.object_name
        parent_name = parents_dict.get(obj_name, "root")
        if parent_name == "root":
            resolved_parent_id = 1  # runtime ROOT_OBJECT = 1
        else:
            resolved_parent_id = id_map[parent_name]

        is_priv = obj_name in privileged_set

        manifest_lines.append("    {")
        manifest_lines.append('        .name = "%s",' % obj_name)
        manifest_lines.append("        .id = %d," % id_map[obj_name])
        manifest_lines.append("        .entry = &%s," % info.entry_point)
        manifest_lines.append("        .parent_id = %d," % resolved_parent_id)
        manifest_lines.append("        .privileged = %s," % ("true" if is_priv else "false"))
        manifest_lines.append("    },")

    manifest_lines.append("};")
    manifest_lines.append("const uint32_t shizuku_composition_manifest_count = %d;" % len(objects))
    manifest_lines.append("")
    manifest_lines.append("// C ABI firmware entry called from SDK boot")
    manifest_lines.append("extern bool shizuku_firmware_load_and_start(const shizuku_object_manifest_entry_t *manifest, uint32_t count);")
    manifest_lines.append("")
    manifest_lines.append("bool shizuku_firmware_entry(void) {")
    manifest_lines.append("    return shizuku_firmware_load_and_start(shizuku_composition_manifest, shizuku_composition_manifest_count);")
    manifest_lines.append("}")
    manifest_lines.append("")

    ctx.actions.write(
        output = manifest_out,
        content = "\n".join(manifest_lines),
    )

    return [
        DefaultInfo(files = depset([ids_out, manifest_out])),
    ]

_shizuku_composition_gen = rule(
    implementation = _shizuku_composition_gen_impl,
    attrs = {
        "objects": attr.label_list(mandatory = True),
        "target": attr.string(default = "esp32c6"),
        "parents": attr.string_dict(default = {}),
        "privileged_names": attr.string_list(default = []),
        "gen_ids_tool": attr.label(default = "@shizuku//tools:gen_object_ids.py", executable = True, allow_single_file = True, cfg = "exec"),
        "source_template": attr.label(default = "@shizuku//tools:object_ids.c.in", allow_single_file = True),
    },
)

# ==============================================================================
# 4. 単一アーカイブバンドル (ar -M による結合)
# ==============================================================================
def _shizuku_firmware_archive_bundle_impl(ctx):
    out_archive = ctx.actions.declare_file("lib" + ctx.label.name + ".a")

    all_deps = [ctx.attr.metadata] + ctx.attr.objects
    if ctx.attr.kernel:
        all_deps.append(ctx.attr.kernel)
    if ctx.attr.kernel_object:
        all_deps.append(ctx.attr.kernel_object)

    archives_dict = {}
    direct_objs_dict = {}

    for dep in all_deps:
        if CcInfo in dep:
            cc_info = dep[CcInfo]
            for li in cc_info.linking_context.linker_inputs.to_list():
                for lib in li.libraries:
                    if lib.static_library:
                        # Dedup archives by artifact path only
                        archives_dict[lib.static_library.path] = lib.static_library
                    else:
                        # Never add both static_library and its own objects into MRI archive
                        objs = lib.pic_objects if lib.pic_objects else lib.objects
                        if objs:
                            for o in objs:
                                direct_objs_dict[o.path] = o

    mri_lines = [
        "CREATE " + out_archive.path,
    ]
    for p in archives_dict:
        mri_lines.append("ADDLIB " + p)
    for p in direct_objs_dict:
        mri_lines.append("ADDMOD " + p)
    mri_lines.append("SAVE")
    mri_lines.append("END")

    mri_file = ctx.actions.declare_file(ctx.label.name + "_merge.mri")
    ctx.actions.write(
        output = mri_file,
        content = "\n".join(mri_lines) + "\n",
    )

    cc_toolchain = ctx.attr._cc_toolchain[cc_common.CcToolchainInfo]
    ar_tool = cc_toolchain.ar_executable

    ctx.actions.run_shell(
        outputs = [out_archive],
        inputs = depset(list(archives_dict.values()) + list(direct_objs_dict.values()) + [mri_file], transitive = [cc_toolchain.all_files]),
        command = "$1 -M < $2",
        arguments = [ar_tool, mri_file.path],
        mnemonic = "MergeFirmwareArchive",
        progress_message = "Merging firmware archive %s" % out_archive.short_path,
    )

    return [
        DefaultInfo(files = depset([out_archive])),
    ]

_shizuku_firmware_archive_bundle = rule(
    implementation = _shizuku_firmware_archive_bundle_impl,
    attrs = {
        "kernel": attr.label(mandatory = False, providers = [CcInfo]),
        "kernel_object": attr.label(mandatory = False, providers = [CcInfo]),
        "objects": attr.label_list(mandatory = True, providers = [CcInfo]),
        "metadata": attr.label(mandatory = True, providers = [CcInfo]),
        "target": attr.string(default = "esp32c6"),
        "_cc_toolchain": attr.label(default = "@bazel_tools//tools/cpp:current_cc_toolchain"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    cfg = _firmware_transition,
)

# ==============================================================================
# 5. Top-level Firmware Binary Rules (with Transition to prevent host-default skip)
# ==============================================================================
def _shizuku_firmware_binary_impl(ctx):
    out_bin = ctx.actions.declare_file(ctx.label.name + ".bin")
    out_elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    out_map = ctx.actions.declare_file(ctx.label.name + ".map")
    archive = ctx.file.archive
    project_files = ctx.files.esp32c6_project
    build_sh = ctx.executable.build_sh

    args = ctx.actions.args()
    args.add(archive.path)
    args.add(out_bin.path)
    args.add("esp-15.2.0_20251204")
    args.add(ctx.attr.target)
    if ctx.attr.build_dir:
        args.add(ctx.attr.build_dir)

    ctx.actions.run(
        outputs = [out_bin, out_elf, out_map],
        inputs = [archive] + project_files,
        executable = build_sh,
        arguments = [args],
        mnemonic = "IdfFinalLink",
        progress_message = "Linking ESP-IDF firmware binary for %s" % ctx.label,
        use_default_shell_env = True,
        execution_requirements = {
            "local": "1",
            "no-sandbox": "1",
        },
    )

    return [DefaultInfo(files = depset([out_bin, out_elf, out_map]))]

_shizuku_firmware_binary = rule(
    implementation = _shizuku_firmware_binary_impl,
    attrs = {
        "archive": attr.label(mandatory = True, allow_single_file = True),
        "target": attr.string(default = "esp32c6"),
        "build_dir": attr.string(default = ""),
        "esp32c6_project": attr.label(default = "@shizuku//modules/esp_idf_support/firmware:esp32c6_project"),
        "build_sh": attr.label(default = "@shizuku//modules/esp_idf_support/firmware:build_sh", executable = True, cfg = "exec"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    cfg = _firmware_transition,
)

def _rp_firmware_binary_impl(ctx):
    # Enforce board == "pico" MVP fail-closed
    if ctx.attr.board != "pico":
        fail("Unsupported board '%s' for RP2040 firmware. Only 'pico' is supported." % ctx.attr.board)

    out_uf2 = ctx.actions.declare_file(ctx.label.name + ".uf2")
    out_elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    out_map = ctx.actions.declare_file(ctx.label.name + ".map")
    archive = ctx.file.archive
    config_header = ctx.file.config_header
    project_files = ctx.files.rp2040_project
    build_sh = ctx.executable.build_sh

    args = ctx.actions.args()
    args.add(archive.path)
    args.add(out_uf2.path)
    args.add(out_elf.path)
    args.add(out_map.path)
    args.add(ctx.attr.pico_sdk_path)
    args.add(ctx.attr.target)
    args.add(ctx.attr.board)
    args.add(ctx.attr.build_dir)
    args.add(config_header.path)

    # Dependency closure: declare CMake project, extra sources, headers, and linker script
    inputs = depset(
        direct = [archive, config_header] + project_files + ctx.files.extra_sources + ctx.files.linker_script,
        transitive = [
            dep[DefaultInfo].files for dep in ctx.attr.extra_headers if DefaultInfo in dep
        ] + [
            dep[CcInfo].compilation_context.headers for dep in ctx.attr.extra_headers if CcInfo in dep
        ],
    )

    ctx.actions.run(
        outputs = [out_uf2, out_elf, out_map],
        inputs = inputs,
        executable = build_sh,
        arguments = [args],
        mnemonic = "PicoSdkFinalLink",
        progress_message = "Linking RP2040 Pico SDK UF2/ELF/MAP firmware for %s" % ctx.label,
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([out_uf2, out_elf, out_map]))]

_rp_firmware_binary = rule(
    implementation = _rp_firmware_binary_impl,
    attrs = {
        "archive": attr.label(mandatory = True, allow_single_file = True),
        "target": attr.string(default = "rp2040"),
        "board": attr.string(default = "pico"),
        "pico_sdk_path": attr.string(default = ""),
        "build_dir": attr.string(default = ""),
        "config_header": attr.label(default = "@shizuku//configs:config_hpp", allow_single_file = True),
        "rp2040_project": attr.label(default = "@shizuku//modules/pico_sdk_support/firmware:rp2040_project"),
        "extra_sources": attr.label_list(
            default = [
                "@shizuku//modules/pico_sdk_support/rp2040:arch_glue_src",
                "@shizuku//boards/rp2040_pico_w:board_src",
                "@shizuku//modules/pico_sdk_support/objects:usb_cdc.cpp",
            ],
            allow_files = True,
        ),
        "extra_headers": attr.label_list(
            default = [
                "@shizuku//:repo_internal_headers",
                "@shizuku//modules/pico_sdk_support:objects_internal_headers",
                "@shizuku//chips/rp2040:headers",
                "@shizuku//boards/rp2040_pico_w:headers",
                "@shizuku//arch/armv6m:headers",
            ],
        ),
        "linker_script": attr.label(
            default = "@shizuku//modules/pico_sdk_support/firmware:rp2040_flash_limit.ld",
            allow_single_file = True,
        ),
        "build_sh": attr.label(default = "@shizuku//modules/pico_sdk_support/firmware:build_sh", executable = True, cfg = "exec"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    cfg = _firmware_transition,
)

# ==============================================================================
# 6. shizuku_firmware マクロ
# ==============================================================================
def shizuku_firmware(
        name,
        target = "esp32c6",
        objects = [],
        parents = {},
        privileged_names = [],
        kernel = None,
        kernel_object = None,
        board = "pico",
        pico_sdk_path = "",
        build_dir = "",
        config_header = "@shizuku//configs:config_hpp",
        **kwargs):
    """合成時ID採番・親子/権限・chip、単一の .a および最終リンクバイナリを生成する。"""
    if target == "rp2350":
        fail("Target 'rp2350' is unsupported (RP2350 is a follow-up and must not be declared supported; only 'rp2040' is supported).")
    if target not in ["esp32c6", "esp32c5", "rp2040"]:
        fail("Unknown target: '%s'. Supported targets are 'esp32c6', 'esp32c5', 'rp2040' ('rp2350' is unsupported)." % target)

    if target == "rp2040":
        if board != "pico":
            fail("Unsupported board '%s' for RP2040 firmware. Only 'pico' is currently supported (pico_w is not yet supported for standalone composition)." % board)

    gen_name = name + "_composition_gen"
    meta_name = name + "_composition_meta"
    archive_name = name + "_archive"

    if kernel == None:
        if target == "esp32c6":
            kernel = "@shizuku//:shizuku_kernel_c6_core"
        elif target == "esp32c5":
            kernel = "@shizuku//:shizuku_kernel_c5"
        elif target == "rp2040":
            kernel = "@shizuku//:shizuku_kernel_rp2040_core"
        else:
            kernel = "@shizuku//:kernel"

    # 1. ID ソースと Manifest ソースを生成 (親解決・サイクル検査・権限設定)
    _shizuku_composition_gen(
        name = gen_name,
        objects = objects,
        target = target,
        parents = parents,
        privileged_names = privileged_names,
        tags = kwargs.get("tags", []),
    )

    # 2. 生成ソースをコンパイル (stdint/stdbool のみを使用し、legacy global ID を引かないよう internal_headers 依存を排除)
    cc_library(
        name = meta_name,
        srcs = [":" + gen_name],
        tags = kwargs.get("tags", []),
    )

    # 3. 単一 .a にバンドル
    _shizuku_firmware_archive_bundle(
        name = archive_name,
        target = target,
        kernel = kernel,
        kernel_object = kernel_object,
        objects = objects,
        metadata = ":" + meta_name,
        tags = kwargs.get("tags", []),
        visibility = kwargs.get("visibility", ["//visibility:public"]),
    )

    # 4. トップレベル最終バイナリ (transition 付きルールでラップして host-default skip を防止)
    if target in ["esp32c6", "esp32c5"]:
        _shizuku_firmware_binary(
            name = name,
            archive = ":" + archive_name,
            target = target,
            build_dir = build_dir,
            tags = kwargs.get("tags", []),
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )
    elif target == "rp2040":
        _rp_firmware_binary(
            name = name,
            archive = ":" + archive_name,
            target = target,
            board = board,
            pico_sdk_path = pico_sdk_path,
            build_dir = build_dir,
            config_header = config_header,
            tags = kwargs.get("tags", []),
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )
    else:
        native.alias(
            name = name,
            actual = ":" + archive_name,
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )

# ==============================================================================
# 7. shizuku_sketch & shizuku_sketch_object
# ==============================================================================
def _shizuku_sketch_entry_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "_entry.cpp")
    content = """// Auto-generated sketch entry wrapper for %s
#include <cstdint>

extern void %s(void);
extern void %s(void);
extern "C" void shizuku_yield(void);

extern "C" void %s(void) {
    %s();
    for (;;) {
        %s();
        shizuku_yield();
    }
}
""" % (
        ctx.attr.sketch_name,
        ctx.attr.setup_fn,
        ctx.attr.loop_fn,
        ctx.attr.entry_point,
        ctx.attr.setup_fn,
        ctx.attr.loop_fn,
    )
    ctx.actions.write(output = out, content = content)
    return [DefaultInfo(files = depset([out]))]

_shizuku_sketch_entry = rule(
    implementation = _shizuku_sketch_entry_impl,
    attrs = {
        "sketch_name": attr.string(mandatory = True),
        "entry_point": attr.string(mandatory = True),
        "setup_fn": attr.string(default = "setup"),
        "loop_fn": attr.string(default = "loop"),
    },
)

def shizuku_sketch_object(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        setup_fn = "setup",
        loop_fn = "loop",
        entry_point = None,
        **kwargs):
    """setup()/loop() ラッパーを持つ shizuku_object ターゲットを生成する。

    他の shizuku_firmware の objects 属性にそのまま渡すことができる。
    複数 sketch 共存時のシンボル衝突を防ぐため、setup_fn / loop_fn の別名を指定可能。
    """
    entry_gen_name = name + "_entry_gen"
    ep = entry_point if entry_point else (name + "_main")

    _shizuku_sketch_entry(
        name = entry_gen_name,
        sketch_name = name,
        entry_point = ep,
        setup_fn = setup_fn,
        loop_fn = loop_fn,
        tags = kwargs.get("tags", []),
    )

    shizuku_object(
        name = name,
        srcs = srcs + [":" + entry_gen_name],
        hdrs = hdrs,
        deps = deps + ["@shizuku//:core_headers"],
        entry_point = ep,
        **kwargs
    )

def shizuku_sketch(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        target = "esp32c6",
        board = "pico",
        setup_fn = "setup",
        loop_fn = "loop",
        kernel = None,
        kernel_object = None,
        pico_sdk_path = "",
        build_dir = "",
        config_header = "@shizuku//configs:config_hpp",
        **kwargs):
    """単一スケッチから shizuku_object と shizuku_firmware を 1 行で作成する糖衣マクロ。

    生成されるターゲット:
    - {name}: 最終ファームウェアバイナリ (shizuku_firmware)
    - {name}_obj: 単体スケッチオブジェクト (shizuku_sketch_object)

    【未達事項・技術境界 (Incomplete)】:
    別の shizuku_firmware の objects 属性に本ターゲット (':foo') を直接指定して再利用
    することは未確立（incomplete）。他ファームウェアでのオブジェクト再利用には、
    明示的に ':foo_obj' を指定する必要がある。
    """
    if target == "rp2040":
        if board != "pico":
            fail("Unsupported board '%s' for RP2040 sketch. Only 'pico' is currently supported (pico_w is not yet supported for standalone composition)." % board)

    obj_name = name + "_obj"

    shizuku_sketch_object(
        name = obj_name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        setup_fn = setup_fn,
        loop_fn = loop_fn,
        tags = kwargs.get("tags", []),
    )

    shizuku_firmware(
        name = name,
        target = target,
        board = board,
        objects = [":" + obj_name],
        kernel = kernel,
        kernel_object = kernel_object,
        pico_sdk_path = pico_sdk_path,
        build_dir = build_dir,
        config_header = config_header,
        **kwargs
    )

shizuku_composition_gen = _shizuku_composition_gen
shizuku_firmware_archive_bundle = _shizuku_firmware_archive_bundle
rp_firmware_binary = _rp_firmware_binary
