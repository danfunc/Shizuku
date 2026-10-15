"""Shizuku Object & Firmware Composition API (defs.bzl)

提供ルール・マクロ:
- ShizukuObjectInfo: オブジェクトメタデータ provider
- shizuku_object: firmware/ID 未定の cc_library compile unit
- shizuku_firmware: 合成時 ID 採番・親子・権限・chip 決定、単一 .a および最終 .bin 生成
- shizuku_sketch: setup()/loop() 入口生成糖衣 + firmware 短縮形
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
        "parent": "Parent object name (string or None)",
        "privileged": "Boolean flag indicating privileged execution",
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
        parent = ctx.attr.parent if ctx.attr.parent else None,
        privileged = ctx.attr.privileged,
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
        "parent": attr.string(),
        "privileged": attr.bool(default = False),
        "dep": attr.label(mandatory = True, providers = [CcInfo]),
    },
)

def shizuku_object(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        object_name = None,
        parent = None,
        privileged = False,
        entry_point = None,
        **kwargs):
    """firmware/ID 未定の cc_library compile unit。

    同 chip/toolchain であれば合成依存なく再コンパイル不要。
    """
    impl_name = name + "_impl"
    actual_obj_name = object_name if object_name else name
    ep = entry_point if entry_point else (actual_obj_name + "_main")

    # ユーザーが明示的に指定した deps に加え、個別ID宣言ヘッダへの依存を許容
    cc_library(
        name = impl_name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        **kwargs
    )

    _shizuku_object_marker(
        name = name,
        object_name = actual_obj_name,
        entry_point = ep,
        parent = parent,
        privileged = privileged,
        dep = ":" + impl_name,
        visibility = kwargs.get("visibility", ["//visibility:public"]),
        tags = kwargs.get("tags", []),
    )

# ==============================================================================
# 3. Composition Transition & Metadata Generator
# ==============================================================================
def _firmware_transition_impl(settings, attr):
    target = attr.target
    if target == "esp32c6":
        return {
            "//command_line_option:platforms": str(Label("@shizuku//toolchains/rv32_esp:esp32c6")),
            str(Label("@shizuku//configs:board")): "esp32_c6",
            "//command_line_option:cxxopt": ["-std=c++23"],
            "//command_line_option:copt": ["-Og", "-g3", "-DSHIZUKU_C6_SKELETON_PERMITTED=1"],
            "//command_line_option:strip": "never",
        }
    elif target == "esp32c5":
        return {
            "//command_line_option:platforms": str(Label("@shizuku//toolchains/rv32_esp:esp32c6")),
            str(Label("@shizuku//configs:board")): "esp32_c5",
            "//command_line_option:cxxopt": ["-std=c++23"],
            "//command_line_option:copt": ["-Og", "-g3", "-DSHIZUKU_C5_SKELETON_PERMITTED=1"],
            "//command_line_option:strip": "never",
        }
    return {}

_firmware_transition = transition(
    implementation = _firmware_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        str(Label("@shizuku//configs:board")),
        "//command_line_option:cxxopt",
        "//command_line_option:copt",
        "//command_line_option:strip",
    ],
)

def _shizuku_composition_gen_impl(ctx):
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

    if len(names) > 64:
        fail("Object count exceeds capacity limit (64): got %d" % len(names))

    for obj in objects:
        info = obj[ShizukuObjectInfo]
        if info.parent and info.parent != "root" and info.parent not in seen:
            fail("Missing parent object '%s' for object '%s'" % (info.parent, info.object_name))

    # 2. Composition ID 定義ソースの生成 (tools/gen_object_ids.py 呼び出し)
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

    # 3. Manifest ソースの生成 (ランタイム初期入力テーブル)
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

    # ID は gen_object_ids.py と同じく index 2 から (0=KERNEL, 1=ROOT)
    curr_id = 2
    for obj in objects:
        info = obj[ShizukuObjectInfo]
        manifest_lines.append("    {")
        manifest_lines.append('        .name = "%s",' % info.object_name)
        manifest_lines.append("        .id = %d," % curr_id)
        manifest_lines.append("        .entry = &%s," % info.entry_point)
        manifest_lines.append("        .parent_id = 0,")
        manifest_lines.append("        .privileged = %s," % ("true" if info.privileged else "false"))
        manifest_lines.append("    },")
        curr_id += 1
    manifest_lines.append("};")
    manifest_lines.append("const uint32_t shizuku_composition_manifest_count = %d;" % len(objects))
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
        "gen_ids_tool": attr.label(default = "@shizuku//tools:gen_object_ids.py", executable = True, allow_single_file = True, cfg = "exec"),
        "source_template": attr.label(default = "@shizuku//tools:object_ids.c.in", allow_single_file = True),
    },
)

# ==============================================================================
# 4. 単一アーカイブバンドル (ar -M による結合)
# ==============================================================================
def _shizuku_firmware_archive_bundle_impl(ctx):
    out_archive = ctx.actions.declare_file("lib" + ctx.label.name + ".a")

    all_deps = [ctx.attr.kernel, ctx.attr.metadata] + ctx.attr.objects
    archives = []
    direct_objs = []

    for dep in all_deps:
        if CcInfo in dep:
            cc_info = dep[CcInfo]
            for li in cc_info.linking_context.linker_inputs.to_list():
                for lib in li.libraries:
                    if lib.static_library:
                        archives.append(lib.static_library)
                    if lib.pic_objects:
                        direct_objs.extend(lib.pic_objects)
                    elif lib.objects:
                        direct_objs.extend(lib.objects)

    mri_lines = [
        "CREATE " + out_archive.path,
    ]
    for a in archives:
        mri_lines.append("ADDLIB " + a.path)
    for o in direct_objs:
        mri_lines.append("ADDMOD " + o.path)
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
        inputs = depset(archives + direct_objs + [mri_file], transitive = [cc_toolchain.all_files]),
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
        "kernel": attr.label(mandatory = True, providers = [CcInfo]),
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
# 5. shizuku_firmware マクロ
# ==============================================================================
def shizuku_firmware(name, target = "esp32c6", objects = [], kernel = None, **kwargs):
    """合成時ID採番・親子/権限・chip、単一の .a および最終リンクバイナリを生成する。"""
    gen_name = name + "_composition_gen"
    meta_name = name + "_composition_meta"
    archive_name = name + "_archive"

    if kernel == None:
        if target == "esp32c6":
            kernel = "@shizuku//:shizuku_kernel"
        elif target == "esp32c5":
            kernel = "@shizuku//:shizuku_kernel_c5"
        else:
            kernel = "@shizuku//:kernel"

    # 1. ID ソースと Manifest ソースを生成
    _shizuku_composition_gen(
        name = gen_name,
        objects = objects,
        tags = kwargs.get("tags", []),
    )

    # 2. 生成ソースをコンパイル
    cc_library(
        name = meta_name,
        srcs = [":" + gen_name],
        deps = ["@shizuku//:internal_headers"],
        tags = kwargs.get("tags", []),
    )

    # 3. 単一 .a にバンドル
    _shizuku_firmware_archive_bundle(
        name = archive_name,
        target = target,
        kernel = kernel,
        objects = objects,
        metadata = ":" + meta_name,
        tags = kwargs.get("tags", []),
        visibility = kwargs.get("visibility", ["//visibility:public"]),
    )

    # 4. 最終リンクバイナリ (C6 の場合は IDF build.sh を呼ぶ genrule)
    if target in ["esp32c6", "esp32c5"]:
        native.genrule(
            name = name,
            srcs = [
                ":" + archive_name,
                "@shizuku//modules/esp_idf_support/firmware:esp32c6_project",
            ],
            outs = [name + ".bin"],
            cmd = "ARCHIVE=; for f in $(locations :" + archive_name + "); do case $$f in *.a) ARCHIVE=$$f;; esac; done; " +
                  "$(location @shizuku//modules/esp_idf_support/firmware:build.sh) $$ARCHIVE $@ esp-15.2.0_20251204 " + target,
            tools = ["@shizuku//modules/esp_idf_support/firmware:build.sh"],
            tags = ["local", "no-sandbox"],
            target_compatible_with = ["@platforms//cpu:riscv32"],
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )
    else:
        native.alias(
            name = name,
            actual = ":" + archive_name,
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )

# ==============================================================================
# 6. shizuku_sketch
# ==============================================================================
def _shizuku_sketch_entry_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "_entry.cpp")
    content = """// Auto-generated sketch entry wrapper
extern void setup(void);
extern void loop(void);

extern "C" void %s_main(void) {
    setup();
    for (;;) {
        loop();
    }
}
""" % ctx.attr.sketch_name
    ctx.actions.write(output = out, content = content)
    return [DefaultInfo(files = depset([out]))]

_shizuku_sketch_entry = rule(
    implementation = _shizuku_sketch_entry_impl,
    attrs = {
        "sketch_name": attr.string(mandatory = True),
    },
)

def shizuku_sketch(name, srcs = [], hdrs = [], deps = [], target = "esp32c6", **kwargs):
    """setup()/loop() 入口生成 object 糖衣 + firmware 短縮形。"""
    entry_gen_name = name + "_entry_gen"
    obj_name = name + "_sketch_obj"

    _shizuku_sketch_entry(
        name = entry_gen_name,
        sketch_name = name,
        tags = kwargs.get("tags", []),
    )

    shizuku_object(
        name = obj_name,
        srcs = srcs + [":" + entry_gen_name],
        hdrs = hdrs,
        deps = deps + ["@shizuku//:internal_headers"],
        entry_point = name + "_main",
        tags = kwargs.get("tags", []),
    )

    shizuku_firmware(
        name = name,
        target = target,
        objects = [":" + obj_name],
        **kwargs
    )
